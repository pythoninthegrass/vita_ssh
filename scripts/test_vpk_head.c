/* Host test driver for src/vpk_head.c; scripts/test_vpk_head.py supplies the inputs and the oracle. */
#include "vpk_head.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *in, *out;
    long len;

    if (argc != 4) {
        fprintf(stderr, "usage: %s TEMPLATE CONTENT_ID OUT\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        return 2;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    in = malloc((size_t)len);
    out = malloc((size_t)len);
    if (fread(in, 1, (size_t)len, f) != (size_t)len) {
        return 2;
    }
    fclose(f);
    if (vpk_make_head_bin(in, (size_t)len, argv[2], out) != 0) {
        return 1;
    }
    f = fopen(argv[3], "wb");
    fwrite(out, 1, (size_t)len, f);
    fclose(f);
    return 0;
}
