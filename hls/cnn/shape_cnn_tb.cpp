// C-simulation testbench for shape_cnn on the shape test set.
// Reads cnn_test_vectors.txt (written by cnn/train_cnn.py):
//   N
//   label keras_pred px0 .. px4799        (raw 8-bit grayscale)
// Pixels go through the SAME integer Q8.8 conversion as the board firmware,
// so this run reproduces what the hardware returns. Pass criterion: the
// fixed-point accelerator agrees with the float Keras model on >= 98 %.
#include <fstream>
#include <iomanip>
#include <iostream>

#include "shape_cnn.h"

static const char *NAMES[N_CLASSES] = {"Circle", "Square", "Triangle", "Figure8"};

int main()
{
    std::ifstream f("cnn_test_vectors.txt");
    if (!f) { std::cerr << "cannot open cnn_test_vectors.txt\n"; return 1; }
    int n; f >> n;

    static data_t img[IMG_H * IMG_W];
    int cm[N_CLASSES][N_CLASSES] = {};
    int correct = 0, agree = 0;
    std::ofstream log("csim_cnn_predictions.txt");

    for (int k = 0; k < n; k++) {
        int label, kpred; f >> label >> kpred;
        for (int i = 0; i < IMG_H * IMG_W; i++) {
            int px; f >> px;
            int q = (1024 * px + 127) / 255 - 512;      // board: Q8.8 int16
            img[i].range(15, 0) = (ap_int<16>)q;
        }
        int pred = -1, logits[N_CLASSES];
        shape_cnn(img, &pred, logits);
        if (pred < 0 || pred >= N_CLASSES) { std::cerr << "bad pred\n"; return 1; }

        cm[label][pred]++;
        correct += (pred == label);
        agree += (pred == kpred);
        log << label << " " << kpred << " " << pred;
        for (int c = 0; c < N_CLASSES; c++) log << " " << logits[c] / 256.0;
        log << "\n";
        if (k % 200 == 0) std::cout << "image " << k << "/" << n << std::endl;
    }

    std::cout << std::fixed << std::setprecision(4)
              << "\nHLS CNN accuracy  : " << (double)correct / n << " (" << correct << "/" << n << ")"
              << "\nagreement w/ Keras: " << (double)agree / n << " (" << agree << "/" << n << ")\n"
              << "\nconfusion (rows=true, cols=pred)\n";
    for (int i = 0; i < N_CLASSES; i++) {
        std::cout << std::setw(9) << NAMES[i];
        for (int j = 0; j < N_CLASSES; j++) std::cout << std::setw(6) << cm[i][j];
        std::cout << "\n";
    }
    return (agree >= n * 0.98) ? 0 : 1;
}
