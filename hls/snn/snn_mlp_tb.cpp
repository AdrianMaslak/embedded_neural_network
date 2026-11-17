// C-simulation testbench for snn_mlp on the shape test set.
// Reads snn_test_vectors.txt (written by snn/train_snn.py):
//   N
//   label ref_pred ref_l0 ref_l1 ref_l2 ref_l3 f0 .. f383     (int16 features)
// The Python integer reference reproduces the kernel arithmetic exactly,
// so prediction AND all four membrane scores must match bit for bit.
#include <fstream>
#include <iostream>
#include <iomanip>

#include "ap_int.h"
#include "fpga_weights.h"

typedef ap_int<16> data_t;
typedef ap_int<32> acc_t;

void snn_mlp(volatile data_t *feature_mem, volatile ap_uint<8> *prediction,
             volatile acc_t logits_out[NUM_CLASSES]);

static const char *NAMES[NUM_CLASSES] = {"Circle", "Square", "Triangle", "Figure8"};

int main()
{
    std::ifstream f("snn_test_vectors.txt");
    if (!f) { std::cerr << "cannot open snn_test_vectors.txt\n"; return 1; }

    int n; f >> n;
    static data_t feat[T_STEPS * N_FEATURES];
    int cm[NUM_CLASSES][NUM_CLASSES] = {};
    int correct = 0, exact = 0;

    for (int k = 0; k < n; k++) {
        int label, rpred; long rl[NUM_CLASSES];
        f >> label >> rpred;
        for (int c = 0; c < NUM_CLASSES; c++) f >> rl[c];
        for (int i = 0; i < T_STEPS * N_FEATURES; i++) { int v; f >> v; feat[i] = v; }

        ap_uint<8> pred = 0;
        acc_t logits[NUM_CLASSES];
        snn_mlp(feat, &pred, logits);

        bool same = ((int)pred == rpred);
        for (int c = 0; c < NUM_CLASSES; c++) same &= ((long)logits[c] == rl[c]);
        exact += same;
        if (!same && exact + 5 > k)   // print first few mismatches
            std::cout << "mismatch img " << k << " pred " << (int)pred << " ref " << rpred << "\n";

        cm[label][(int)pred]++;
        correct += ((int)pred == label);
    }

    std::cout << std::fixed << std::setprecision(4)
              << "\nHLS SNN accuracy        : " << (double)correct / n << " (" << correct << "/" << n << ")"
              << "\nbit-exact vs int ref    : " << exact << "/" << n << "\n"
              << "\nconfusion (rows=true, cols=pred)\n";
    for (int i = 0; i < NUM_CLASSES; i++) {
        std::cout << std::setw(9) << NAMES[i];
        for (int j = 0; j < NUM_CLASSES; j++) std::cout << std::setw(6) << cm[i][j];
        std::cout << "\n";
    }
    return (exact == n) ? 0 : 1;
}
