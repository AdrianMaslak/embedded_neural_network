#ifndef PREPROCESS_H
#define PREPROCESS_H
#include <stdint.h>

#define SNN_GRID        16
#define SNN_N_FEATURES  (SNN_GRID * SNN_GRID * 2)
#define SNN_EDGE_DIV    3
#define SNN_COUNT_CLIP  4
#define SNN_FEAT_SCALE  128

/* CNN input: 8-bit pixel -> Q8.8 of (x/255 - 0.5)/0.25, integer rounding. */
void cnn_preprocess(const uint8_t *img, int16_t *q88);

/* SNN input: Sobel edge spikes -> 512 int16 features (0..128).
 * Bit-exact mirror of common.py: snn_features_int16(). */
void snn_preprocess(const uint8_t *img, int16_t *feat);

uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, uint32_t len);

#endif
