/*
 * Host check: firmware/preprocess.c (compiled natively) vs Python reference.
 * Reads the raw test images from hls/cnn/cnn_test_vectors.txt and the Python
 * SNN features from hls/snn/snn_test_vectors.txt (same image order) and
 * requires every one of the 512 features of every image to match.
 *
 *   gcc -O2 -I../firmware check_preprocess.c ../firmware/preprocess.c -o check
 *   ./check ../hls/cnn/cnn_test_vectors.txt ../hls/snn/snn_test_vectors.txt
 */
#include <stdio.h>
#include <string.h>
#include "preprocess.h"
#include "protocol.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: check cnn_vectors snn_vectors\n"); return 2; }
    FILE *fc = fopen(argv[1], "r"), *fs = fopen(argv[2], "r");
    if (!fc || !fs) { fprintf(stderr, "cannot open inputs\n"); return 2; }

    /* CRC-16/CCITT-FALSE check value */
    uint16_t crc = crc16_ccitt(0xFFFF, (const uint8_t *)"123456789", 9);
    printf("crc16(\"123456789\") = 0x%04X (expect 0x29B1)\n", crc);

    int n1, n2;
    if (fscanf(fc, "%d", &n1) != 1 || fscanf(fs, "%d", &n2) != 1 || n1 != n2) return 2;

    static uint8_t img[IMG_BYTES];
    static int16_t feat[SNN_N_FEATURES];
    int bad_imgs = 0;
    for (int k = 0; k < n1; k++) {
        int label, pred, v;
        if (fscanf(fc, "%d %d", &label, &pred) != 2) return 2;
        for (int i = 0; i < IMG_BYTES; i++) { if (fscanf(fc, "%d", &v) != 1) return 2; img[i] = (uint8_t)v; }

        int l2, p2; long lg;
        if (fscanf(fs, "%d %d", &l2, &p2) != 2) return 2;
        for (int c = 0; c < 4; c++) if (fscanf(fs, "%ld", &lg) != 1) return 2;

        snn_preprocess(img, feat);
        int diff = 0;
        for (int i = 0; i < SNN_N_FEATURES; i++) {
            if (fscanf(fs, "%d", &v) != 1) return 2;
            diff += (feat[i] != v);
        }
        if (diff) { if (bad_imgs < 5) printf("image %d: %d features differ\n", k, diff); bad_imgs++; }
    }
    printf("SNN preprocessing: %d / %d images bit-exact\n", n1 - bad_imgs, n1);
    return (bad_imgs == 0 && crc == 0x29B1) ? 0 : 1;
}
