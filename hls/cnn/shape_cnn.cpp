// ShapeCNN inference accelerator (Vitis HLS)
//
//   60x80x1 -B1-> 30x40x8 -B2-> 15x20x16 -B3-> 7x10x16 -B4-> 7x10x32 -GAP-> 32 -FC-> 4
//   B1..B3 = Conv3x3 (same padding) + folded BN + ReLU + MaxPool2, B4 without pool.
//
// Every block is one flattened loop pipelined at II=1. One iteration = one
// 3x3 tap: all input channels are multiplied in parallel (B1: all 8 output
// channels in parallel instead, since it has one input channel). MaxPool is
// fused: each pooled output evaluates its 4 conv positions back to back.
#include "shape_cnn.h"
#include "cnn_weights.h"

static inline data_t relu(acc_t x) { return x > 0 ? (data_t)x : (data_t)0; }

// ---------------------------------------------------------------------------
// B1: 1 -> 8 channels, output channels unrolled (8 MAC / cycle)
// ---------------------------------------------------------------------------
static void block1(data_t in[IMG_H][IMG_W], data_t out[IMG_H / 2][IMG_W / 2][C1])
{
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=C1_W dim=1 complete
#pragma HLS ARRAY_PARTITION variable=C1_B dim=1 complete
    const int OH = IMG_H / 2, OW = IMG_W / 2;
    acc_t acc[C1];
    data_t mx[C1];
#pragma HLS ARRAY_PARTITION variable=acc complete
#pragma HLS ARRAY_PARTITION variable=mx complete

    int ph = 0, pw = 0, p = 0, k = 0;
    for (int it = 0; it < OH * OW * 4 * 9; it++) {
#pragma HLS PIPELINE II=1
        int ih = 2 * ph + (p >> 1) + k / 3 - 1;
        int iw = 2 * pw + (p & 1) + k % 3 - 1;
        data_t x = (ih >= 0 && ih < IMG_H && iw >= 0 && iw < IMG_W) ? in[ih][iw] : (data_t)0;

        for (int co = 0; co < C1; co++) {
            acc_t a = (k == 0) ? (acc_t)C1_B[co] : acc[co];
            a += x * C1_W[co][0][k];
            acc[co] = a;
            if (k == 8) {
                data_t v = relu(a);
                data_t m = (p == 0 || v > mx[co]) ? v : mx[co];
                mx[co] = m;
                if (p == 3) out[ph][pw][co] = m;
            }
        }
        if (++k == 9) { k = 0;
            if (++p == 4) { p = 0;
                if (++pw == OW) { pw = 0; ph++; } } }
    }
}

// ---------------------------------------------------------------------------
// Generic block: CI input channels unrolled (CI MAC / cycle), CO sequential.
// POOL: fused 2x2 max pool (floor), else plain conv.
// ---------------------------------------------------------------------------
template <int H, int W, int CI, int CO, bool POOL>
static void conv_block(data_t in[H][W][CI],
                       data_t out[POOL ? H / 2 : H][POOL ? W / 2 : W][CO],
                       const wt_t wgt[CO][CI][9], const wt_t bias[CO])
{
#pragma HLS INLINE off
#pragma HLS ARRAY_PARTITION variable=wgt dim=2 complete
    const int OH = POOL ? H / 2 : H, OW = POOL ? W / 2 : W, NP = POOL ? 4 : 1;
    acc_t acc = 0;
    data_t mx = 0;

    int oh = 0, ow = 0, co = 0, p = 0, k = 0;
    for (int it = 0; it < OH * OW * CO * NP * 9; it++) {
#pragma HLS PIPELINE II=1
        int ch = POOL ? 2 * oh + (p >> 1) : oh;
        int cw = POOL ? 2 * ow + (p & 1) : ow;
        int ih = ch + k / 3 - 1, iw = cw + k % 3 - 1;
        bool valid = ih >= 0 && ih < H && iw >= 0 && iw < W;

        // products of all input channels summed outside the loop-carried path
        acc_t psum = 0;
        for (int ci = 0; ci < CI; ci++) {
            data_t x = valid ? in[ih][iw][ci] : (data_t)0;
            psum += x * wgt[co][ci][k];
        }
        acc_t a = ((k == 0) ? (acc_t)bias[co] : acc) + psum;
        acc = a;
        if (k == 8) {
            data_t v = relu(a);
            data_t m = (p == 0 || v > mx) ? v : mx;
            mx = m;
            if (p == NP - 1) out[oh][ow][co] = m;
        }
        if (++k == 9) { k = 0;
            if (++p == NP) { p = 0;
                if (++co == CO) { co = 0;
                    if (++ow == OW) { ow = 0; oh++; } } } }
    }
}

// ---------------------------------------------------------------------------
// GAP + fully connected classifier + argmax
// ---------------------------------------------------------------------------
static int classify(data_t f[7][10][C4], int logits_out[N_CLASSES])
{
#pragma HLS INLINE off
    acc_t gap[C4];
#pragma HLS ARRAY_PARTITION variable=gap complete
    for (int c = 0; c < C4; c++) {
#pragma HLS UNROLL
        gap[c] = 0;
    }
    for (int h = 0; h < 7; h++)
        for (int w = 0; w < 10; w++)
            for (int c = 0; c < C4; c++) {
#pragma HLS PIPELINE II=1
                gap[c] += f[h][w][c];
            }

    const acc_t inv70 = (acc_t)(1.0 / 70.0);
    int best = 0;
    acc_t best_v = 0;
    for (int o = 0; o < N_CLASSES; o++) {
        acc_t s = FC_B[o];
        for (int c = 0; c < C4; c++) {
#pragma HLS PIPELINE II=1
            s += (data_t)(gap[c] * inv70) * FC_W[o][c];
        }
        data_t l = s;
        logits_out[o] = (int)(ap_int<16>)l.range(15, 0);   // Q8.8 raw bits
        if (o == 0 || s > best_v) { best_v = s; best = o; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Top level
// ---------------------------------------------------------------------------
void shape_cnn(const data_t *input_img, int *pred_out, int logits_out[N_CLASSES])
{
#pragma HLS INTERFACE m_axi port=input_img offset=slave bundle=gmem depth=4800 max_read_burst_length=256
#pragma HLS INTERFACE s_axilite port=input_img bundle=control
#pragma HLS INTERFACE s_axilite port=pred_out bundle=control
#pragma HLS INTERFACE s_axilite port=logits_out bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    static data_t img[IMG_H][IMG_W];
    static data_t f1[30][40][C1];
    static data_t f2[15][20][C2];
    static data_t f3[7][10][C3];
    static data_t f4[7][10][C4];
#pragma HLS ARRAY_PARTITION variable=f1 dim=3 complete
#pragma HLS ARRAY_PARTITION variable=f2 dim=3 complete
#pragma HLS ARRAY_PARTITION variable=f3 dim=3 complete
#pragma HLS ARRAY_PARTITION variable=f4 dim=3 complete

    for (int i = 0; i < IMG_H * IMG_W; i++) {
#pragma HLS PIPELINE II=1
        img[i / IMG_W][i % IMG_W] = input_img[i];
    }

    block1(img, f1);
    conv_block<30, 40, C1, C2, true>(f1, f2, C2_W, C2_B);
    conv_block<15, 20, C2, C3, true>(f2, f3, C3_W, C3_B);
    conv_block<7, 10, C3, C4, false>(f3, f4, C4_W, C4_B);
    *pred_out = classify(f4, logits_out);
}
