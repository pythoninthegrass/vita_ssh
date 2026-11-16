#!/usr/bin/env python3
"""
Chooses and verifies the words the login passphrase is drawn from.

The server shows a generated passphrase on the Vita's screen when there is no
authorized_keys file: PASSPHRASE_WORDS words from a table in src/sshd_hostkey.c,
with nothing between them. Three properties make that readable and correct, and
all three are arithmetic, so none of them should be trusted to an eye reading a
list of 1024 strings:

  1. COUNT is a power of two. The generator takes WORDLIST_BITS bits per word and
     indexes the table with them, so a table of exactly 2**BITS entries makes every
     word equally likely. Any other length makes selection non-uniform -- which is
     precisely why the alphabet this replaced was indexed with `% alphabet` and was
     silently biased. A table SHORT of 2**BITS is worse than biased: it is a read
     past the end of the array.

  2. No entry is a prefix of another. A passphrase with no separators has to be
     segmented by eye, and a prefix pair is the case where two readings are BOTH
     entirely valid words -- "sandal pine triangle" and "sand alpine triangle" are
     different secrets that can display alike. Every prefix-free code is uniquely
     decodable, so this one rule turns that from a probability into a guarantee.
     (Measured before enforcing it: about 1 passphrase in 15 000 had two valid
     segmentations. After: none in 300 000 draws.)

  3. The buffer is big enough. The longest word times PASSPHRASE_WORDS plus the
     NUL has to fit the field in include/sshd.h, so that check is done here rather
     than left to the runtime guard, which can only fail closed.

  4. The array in the source is what scripts/wordlist-pool.txt produces. --check
     compares it against a fresh selection, so editing the list by hand is an error
     that says "edit the pool, then run --emit" instead of a quiet fork between the
     two. That drift happened while this script was being written, which is the
     argument for making it a check rather than a habit.

Nothing writes to the source. --emit prints an array to paste in; --check verifies
what is already there, and is the mode to run after editing the pool.

Usage:
    scripts/gen_wordlist.py [--check] [SOURCE ...]   # verify; exit 1 on any failure
    scripts/gen_wordlist.py --emit [--pool FILE]     # print a replacement C array
    scripts/gen_wordlist.py --longest                # longest words, for sizing

Args:
    SOURCE: File(s) holding the wordlist array. Default: src/sshd_hostkey.c.
    --pool: Candidate words, one group per line, '#' starts a comment.
            Default: scripts/wordlist-pool.txt.
    --emit: Print the selected 2**BITS words as a C array, from the pool.
    --longest: Print the longest entries and the buffer they need, and stop.
"""

from __future__ import annotations

import argparse
import random
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPO / "src" / "sshd_hostkey.c"
DEFAULT_HEADER = REPO / "include" / "sshd.h"
DEFAULT_POOL = Path(__file__).resolve().parent / "wordlist-pool.txt"

MIN_LEN, MAX_LEN = 3, 9
SHAPE = re.compile(r"^[a-z]{%d,%d}$" % (MIN_LEN, MAX_LEN))
SEED = 20261115  # cosmetic: spreads the pool's category grouping across indices,
                 # so a drawn passphrase is not "six animals in a row". Not secret.

ARRAY_RE = re.compile(
    r"static const char \*const wordlist\[WORDLIST_COUNT\] = \{(.*?)\n\};", re.S)
BITS_RE = re.compile(r"#define\s+WORDLIST_BITS\s+(\d+)")
WORDS_RE = re.compile(r"#define\s+PASSPHRASE_WORDS\s+(\d+)")
FIELD_RE = re.compile(r"char\s+password\[(\d+)\]")


class Check:
    """Collects failures instead of raising on the first: a bad pool usually
    breaks several properties at once and saying all of them is more useful
    than saying one."""

    def __init__(self) -> None:
        self.failures: list[str] = []
        self.notes: list[str] = []

    def fail(self, msg: str) -> None:
        self.failures.append(msg)

    def ok(self, msg: str) -> None:
        self.notes.append(msg)


def requires(path: Path) -> Path:
    if not path.exists():
        sys.exit("%s: no such file" % path)
    return path


def defines(source: Path) -> tuple[int, int]:
    """The counts live in the source, so the checker cannot drift from it."""
    text = requires(source).read_text(encoding="utf-8")
    bits = BITS_RE.search(text)
    words = WORDS_RE.search(text)
    if not bits or not words:
        sys.exit("%s: WORDLIST_BITS or PASSPHRASE_WORDS not found" % source)
    return int(bits.group(1)), int(words.group(1))


def table(source: Path) -> list[str]:
    text = requires(source).read_text(encoding="utf-8")
    body = ARRAY_RE.search(text)
    if not body:
        sys.exit("%s: wordlist array not found" % source)
    # Not "([a-z]*)": a word that has gone out of shape has to survive extraction
    # long enough to be reported, which is the opposite of filtering it out here.
    return re.findall(r'"([^"]*)"', body.group(1))


def pool_words(pool: Path) -> list[str]:
    """Candidates in file order, deduped. Anything out of shape is dropped and
    reported: a nine-and-a-half letter word in the pool should cost that word its
    place, not stop somebody from checking the list."""
    seen, out, dropped = set(), [], []
    for raw in pool.read_text(encoding="utf-8").splitlines():
        for token in raw.split("#", 1)[0].split():
            w = token.strip().lower()
            if w in seen:
                continue
            seen.add(w)
            (out if SHAPE.match(w) else dropped).append(w)
    if dropped:
        preview = ", ".join(dropped[:8])
        print("pool: %d word(s) dropped for shape (>%d letters, or not a-z): %s%s"
              % (len(dropped), MAX_LEN, preview,
                 " ..." if len(dropped) > 8 else ""), file=sys.stderr)
    if len(out) < 1:
        sys.exit("%s: no usable candidates" % pool)
    return out


def select(candidates: list[str], count: int) -> list[str]:
    """Keep candidates that break no prefix rule against what is already kept.

    Cycled length-bucket by length-bucket, longest bucket first. Plain
    longest-first fills every slot before reaching the short words, leaving a
    table with no three-letter entries and a longer passphrase to type for no
    reason anybody asked for. Round-robin lets every length in, and the long
    bucket going first still means the distinctive word wins a prefix conflict:
    "bracket" over "bra", which removes three boundaries a reader can get wrong.
    """
    buckets: dict[int, list[str]] = {}
    for w in candidates:
        buckets.setdefault(len(w), []).append(w)
    for bucket in buckets.values():
        bucket.sort()

    kept: list[str] = []
    index: set[str] = set()
    cursor: dict[int, int] = {}
    lengths = sorted(buckets, reverse=True)
    while len(kept) < count and any(
            cursor.get(L, 0) < len(buckets[L]) for L in lengths):
        for L in lengths:
            bucket = buckets[L]
            i = cursor.get(L, 0)
            while i < len(bucket):
                w = bucket[i]
                i += 1
                if any(w.startswith(k) or k.startswith(w) for k in index):
                    continue
                kept.append(w)
                index.add(w)
                break
            # Recorded whether or not this pass accepted, so a bucket holding
            # only conflicts still makes progress instead of spinning here.
            cursor[L] = i
            if len(kept) == count:
                break
    if len(kept) < count:
        sys.exit("pool yields only %d prefix-free words, need %d: add candidates "
                 "to the pool" % (len(kept), count))
    random.Random(SEED).shuffle(kept)
    return kept


def spellable(word: str, words: set[str]) -> bool:
    """True if word splits into two or more other entries. The other way a
    boundary can be read twice; prefix-freeness already forbids it, and this is
    asserted anyway because it is cheap and it is the property that matters."""
    n = len(word)
    reach = [False] * (n + 1)
    reach[0] = True
    for i in range(1, n + 1):
        for j in range(i):
            if reach[j] and word[j:i] in words and not (j == 0 and i == n):
                reach[i] = True
                break
    return reach[n]


def verify(words: list[str], bits: int, passphrase_words: int,
           bufsize: int, check: Check) -> None:
    count = 1 << bits
    if len(words) != count:
        check.fail("%d entries, need exactly %d (2**%d): the generator indexes "
                   "with %d random bits, so a short table reads past the end"
                   % (len(words), count, bits, bits))
    if len(set(words)) != len(words):
        dupes = sorted({w for w in words if words.count(w) > 1})
        check.fail("%d duplicate word(s): %s" % (len(dupes), ", ".join(dupes[:10])))
    bad = [w for w in words if not SHAPE.match(w)]
    if bad:
        check.fail("%d word(s) outside [a-z]{%d,%d}: %s"
                   % (len(bad), MIN_LEN, MAX_LEN, ", ".join(bad[:10])))

    words_set = set(words)
    pairs = [(a, b) for a in words_set for b in words_set
             if a != b and b.startswith(a)]
    if pairs:
        check.fail("NOT prefix-free, %d pair(s): %s"
                   % (len(pairs), ", ".join("%s/%s" % p for p in pairs[:6])))
    spelled = [w for w in sorted(words_set) if spellable(w, words_set)]
    if spelled:
        check.fail("spellable from other entries: %s" % ", ".join(spelled[:10]))

    longest = max((len(w) for w in words), default=0)
    need = passphrase_words * longest + 1
    if need > bufsize:
        check.fail("password[N] is %d but %d words of %d letters need %d"
                   % (bufsize, passphrase_words, longest, need))
    # Recorded unconditionally and filtered where it is printed: a later check can
    # still fail, and a healthy-looking summary beside a failure is the line that
    # gets skimmed and trusted.
    check.ok("%d entries, %d bits, longest %d letters -> %d of %d byte buffer, "
             "mean %.1f chars to type"
             % (len(words), bits, longest, need, bufsize,
                passphrase_words * sum(len(w) for w in words) / float(len(words))))


def header_buffer(header: Path) -> int:
    text = requires(header).read_text(encoding="utf-8")
    field = FIELD_RE.search(text)
    if not field:
        sys.exit("%s: char password[N] not found" % header)
    return int(field.group(1))


def compare_with_pool(words: list[str], pool: Path, count: int,
                      check: Check) -> None:
    """The array must be what the pool produces, or the pool is decoration.

    Without this, --check certifies a table nobody can regenerate: editing the
    array by hand passes every property check while scripts/wordlist-pool.txt
    quietly stops being its source. That drift happened while writing this
    script, which is the argument for making it an error rather than a habit.
    """
    if not pool.exists():
        check.fail("%s missing: the array is only verifiable against its pool"
                   % pool)
        return
    expected = select(pool_words(pool), count)
    missing = sorted(set(expected) - set(words))
    extra = sorted(set(words) - set(expected))
    if missing or extra:
        check.fail("array is not what %s produces: %d missing (%s), %d extra (%s)"
                   " -- edit the pool, then run --emit"
                   % (pool.name, len(missing), ", ".join(missing[:6]),
                      len(extra), ", ".join(extra[:6])))
    elif words != expected:
        check.fail("array holds the pool's %d words in an order --emit does not "
                   "produce; run --emit" % count)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("source", nargs="*", type=Path,
                    help="file(s) holding the wordlist array")
    ap.add_argument("--pool", type=Path, default=DEFAULT_POOL)
    ap.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    ap.add_argument("--check", action="store_true",
                    help="verify the array in place; this is also the default")
    ap.add_argument("--emit", action="store_true",
                    help="print a replacement array instead of checking")
    ap.add_argument("--longest", action="store_true",
                    help="print the longest entries and the buffer they need")
    args = ap.parse_args(argv)

    sources = args.source or [DEFAULT_SOURCE]
    bits, passphrase_words = defines(sources[0])

    if args.emit:
        count = 1 << bits
        words = select(pool_words(args.pool), count)
        for i in range(0, count, 8):
            print("    " + " ".join('"%s",' % w for w in words[i:i + 8]))
        check = Check()
        verify(words, bits, passphrase_words, header_buffer(args.header), check)
        for note in check.notes:
            print("/* %s */" % note, file=sys.stderr)
        for failure in check.failures:
            print("/* FAIL: %s */" % failure, file=sys.stderr)
        return 1 if check.failures else 0

    status = 0
    for source in sources:
        check = Check()
        words = table(source)
        verify(words, bits, passphrase_words, header_buffer(args.header), check)
        compare_with_pool(words, args.pool, 1 << bits, check)
        if args.longest:
            for w in sorted(set(words), key=len, reverse=True)[:12]:
                print("%d %s" % (len(w), w))
        if check.failures:
            for failure in check.failures:
                print("%s: FAIL: %s" % (source.name, failure), file=sys.stderr)
            status = 1
        else:
            for note in check.notes:
                print("%s: %s" % (source.name, note))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
