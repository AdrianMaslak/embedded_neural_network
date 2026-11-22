/*
 * CPU-side preprocessing. Must stay bit-exact with assignment2/common.py.
 * Checked on the PC by tools/check_preprocess (same source compiled natively).
 */
#include "preprocess.h"
#include "protocol.h"

void cnn_preprocess(const uint8_t *img, int16_t *q88)
{
    for (int i = 0; i < IMG_BYTES; i++)
        q88[i] = (int16_t)((1024 * (int32_t)img[i] + 127) / 255 - 512);
}

/* cv2 BORDER_REFLECT_101: -1 -> 1, n -> n-2 */
static inline int refl(int i, int n)
{
    if (i < 0) return -i;
    if (i >= n) return 2 * n - 2 - i;
    return i;
}

void snn_preprocess(const uint8_t *img, int16_t *feat)
{
    static uint16_t mag[IMG_BYTES];
    static uint8_t  dir[IMG_BYTES];      /* 0: |gx| >= |gy|, 1: otherwise */
    int32_t maxmag = 0;

    /* 3x3 Sobel, exactly cv2.Sobel(img, CV_16S, 1,0 / 0,1, ksize=3) */
    for (int y = 0; y < IMG_H; y++) {
        const uint8_t *rm = img + refl(y - 1, IMG_H) * IMG_W;
        const uint8_t *r0 = img + y * IMG_W;
        const uint8_t *rp = img + refl(y + 1, IMG_H) * IMG_W;
        for (int x = 0; x < IMG_W; x++) {
            int xm = refl(x - 1, IMG_W), xp = refl(x + 1, IMG_W);
            int32_t gx = (rm[xp] + 2 * r0[xp] + rp[xp]) - (rm[xm] + 2 * r0[xm] + rp[xm]);
            int32_t gy = (rp[xm] + 2 * rp[x] + rp[xp]) - (rm[xm] + 2 * rm[x] + rm[xp]);
            int32_t ax = gx < 0 ? -gx : gx, ay = gy < 0 ? -gy : gy;
            int32_t m = ax + ay;
            mag[y * IMG_W + x] = (uint16_t)m;
            dir[y * IMG_W + x] = (ax >= ay) ? 0 : 1;
            if (m > maxmag) maxmag = m;
        }
    }

    /* spikes + bounding box */
    int x0 = IMG_W, x1 = -1, y0 = IMG_H, y1 = -1;
    for (int i = 0; i < IMG_BYTES; i++) {
        if ((int32_t)mag[i] * SNN_EDGE_DIV > maxmag) {
            int y = i / IMG_W, x = i % IMG_W;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        } else {
            dir[i] = 2;                          /* no spike */
        }
    }

    int ox = 0, oy = 0, S = (IMG_H > IMG_W) ? IMG_H : IMG_W;
    if (x1 >= 0) {
        int w = x1 - x0 + 1, h = y1 - y0 + 1;
        S = (w > h) ? w : h;
        ox = x0 - (S - w) / 2;
        oy = y0 - (S - h) / 2;
    }

    /* bin spikes on a GRID x GRID raster over the bounding square */
    uint8_t cnt[SNN_N_FEATURES] = {0};
    for (int i = 0; i < IMG_BYTES; i++) {
        if (dir[i] == 2) continue;
        int y = i / IMG_W, x = i % IMG_W;
        int cx = (x - ox) * SNN_GRID / S;
        int cy = (y - oy) * SNN_GRID / S;
        if (cx < 0) cx = 0;
        if (cx > SNN_GRID - 1) cx = SNN_GRID - 1;
        if (cy < 0) cy = 0;
        if (cy > SNN_GRID - 1) cy = SNN_GRID - 1;
        int idx = (cy * SNN_GRID + cx) * 2 + dir[i];
        if (cnt[idx] < SNN_COUNT_CLIP) cnt[idx]++;
    }
    for (int i = 0; i < SNN_N_FEATURES; i++)
        feat[i] = (int16_t)(cnt[i] * (SNN_FEAT_SCALE / SNN_COUNT_CLIP));
}

uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, uint32_t len)
{
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
