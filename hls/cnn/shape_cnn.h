#pragma once
#include "ap_fixed.h"
#include "ap_int.h"

// Activations / input: Q8.8 (same bits the CPU writes to DDR), rounded + saturated
typedef ap_fixed<16, 8, AP_RND, AP_SAT> data_t;
// BN-folded weights: 18 bit (fits one DSP48 port), range +-32, resolution 2^-12
typedef ap_fixed<18, 6, AP_RND, AP_SAT> wt_t;
// Accumulator
typedef ap_fixed<32, 14> acc_t;

static const int IMG_H = 60, IMG_W = 80;
static const int N_CLASSES = 4;
static const int C1 = 8, C2 = 16, C3 = 16, C4 = 32;

// in: 4800 Q8.8 pixels in DDR (m_axi). out: class index + 4 logits (Q8.8 as int)
void shape_cnn(const data_t *input_img, int *pred_out, int logits_out[N_CLASSES]);
