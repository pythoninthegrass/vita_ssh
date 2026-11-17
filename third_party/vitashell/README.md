# VitaShell resources

`head.bin` is copied unmodified from [TheOfficialFloW/VitaShell](https://github.com/TheOfficialFloW/VitaShell), `resources/head.bin`, last changed upstream in commit `81204866399bc939c71b28c42b8c9a8881728657` (2017-03-05). SHA-256 `cbb88299319048e19115a1fc9c76b04e33745291749de0e08963b9b425623f45`. `LICENSE` is VitaShell's licence file, the GNU General Public License version 3.

This directory is **not** covered by the repository's Unlicense dedication; both files stay under VitaShell's GPL-3.0 terms.

What it is used for: `head.bin` is the template for the `sce_sys/package/head.bin` that the Vita's package installer (`scePromoterUtilityPromotePkgWithRif`) requires next to an unpacked VPK. `CMakeLists.txt` compiles it into the server app, and `src/vpk_head.c` writes the content ID into a copy and patches the three MACs the header carries, following the procedure in VitaShell's `package_installer.c` (`makeHeadBin`, `fpkg_hmac`). That code is written for this repo and verified against an independent transcription in `scripts/test_vpk_head.py`.
