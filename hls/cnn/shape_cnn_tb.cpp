// C-simulation testbench for shape_cnn on the shape test set.
// Reads cnn_test_vectors.txt (written by cnn/train_cnn.py):
//   N
//   label keras_pred px0 .. px4799        (raw 8-bit grayscale)
// Pixels go through the SAME integer Q8.8 conversion as the board firmware,
// so this run reproduces what the hardware returns. Pass criterion: the
// fixed-point accelerator agrees with the float Keras model on >= 98 %.
//
// Image range (so the 1200 images can be simulated in chunks):
//   argv:  shape_cnn_tb [start] [count]
//   env:   CNN_TB_START, CNN_TB_COUNT  (used when no argv is given)
//   default: all images. run_hls.tcl passes HLS_CSIM_START / HLS_CSIM_COUNT.
// Results of each run: csim_cnn_predictions_<start>_<end>.txt (one line per
// image, written as it goes) and csim_cnn_summary_<start>_<end>.txt.
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "shape_cnn.h"

static const char *NAMES[N_CLASSES] = {"Circle", "Square", "Triangle", "Figure8"};
static const int N_PX = IMG_H * IMG_W;

static int arg_or_env(int argc, char **argv, int idx, const char *env, int dflt)
{
    if (argc > idx) return std::atoi(argv[idx]);
    const char *e = std::getenv(env);
    return (e && *e) ? std::atoi(e) : dflt;
}

int main(int argc, char **argv)
{
    std::ifstream f("cnn_test_vectors.txt");
    if (!f) { std::cerr << "cannot open cnn_test_vectors.txt\n"; return 1; }
    int n_file; f >> n_file;

    int start = arg_or_env(argc, argv, 1, "CNN_TB_START", 0);
    int count = arg_or_env(argc, argv, 2, "CNN_TB_COUNT", n_file);
    if (start < 0) start = 0;
    if (start > n_file) start = n_file;
    if (count < 0 || start + count > n_file) count = n_file - start;
    const int end = start + count;
    if (count == 0) { std::cerr << "no images selected\n"; return 1; }

    std::cout << "CNN C-sim: images " << start << ".." << end - 1
              << " (" << count << " of " << n_file << ")" << std::endl;

    // skip the images before 'start' without converting them
    std::string line;
    std::getline(f, line);                       // rest of the header line
    for (int k = 0; k < start; k++) std::getline(f, line);

    static data_t img[N_PX];
    int cm[N_CLASSES][N_CLASSES] = {};
    int correct = 0, agree = 0, keras_correct = 0;

    std::ostringstream tag;
    tag << start << "_" << end;
    std::ofstream log(("csim_cnn_predictions_" + tag.str() + ".txt").c_str());
    log << "# index label keras_pred hls_pred logit0..3 (Q8.8 -> float)\n";

    for (int k = start; k < end; k++) {
        int label, kpred;
        if (!(f >> label >> kpred)) { std::cerr << "read error at image " << k << "\n"; return 1; }
        for (int i = 0; i < N_PX; i++) {
            int px; f >> px;
            int q = (1024 * px + 127) / 255 - 512;      // board: Q8.8 int16
            img[i].range(15, 0) = (ap_int<16>)q;
        }
        int pred = -1, logits[N_CLASSES];
        shape_cnn(img, &pred, logits);
        if (pred < 0 || pred >= N_CLASSES) { std::cerr << "bad pred at image " << k << "\n"; return 1; }

        cm[label][pred]++;
        correct += (pred == label);
        agree += (pred == kpred);
        keras_correct += (kpred == label);
        log << k << " " << label << " " << kpred << " " << pred;
        for (int c = 0; c < N_CLASSES; c++) log << " " << logits[c] / 256.0;
        log << std::endl;                            // flushed: survives a crash
        int done = k - start + 1;
        if (done % 50 == 0 || k == end - 1)
            std::cout << "image " << done << "/" << count
                      << "  correct " << correct << "  agree " << agree << std::endl;
    }

    std::ostringstream s;
    s << std::fixed << std::setprecision(4)
      << "CNN C-simulation, test images " << start << ".." << end - 1 << " (" << count << ")\n"
      << "HLS CNN accuracy   : " << (double)correct / count << " (" << correct << "/" << count << ")\n"
      << "Keras accuracy     : " << (double)keras_correct / count << " (" << keras_correct << "/" << count << ")\n"
      << "agreement w/ Keras : " << (double)agree / count << " (" << agree << "/" << count << ")\n"
      << "\nconfusion (rows=true, cols=pred)\n" << std::setw(9) << "";
    for (int j = 0; j < N_CLASSES; j++) s << std::setw(9) << NAMES[j];
    s << "\n";
    for (int i = 0; i < N_CLASSES; i++) {
        s << std::setw(9) << NAMES[i];
        for (int j = 0; j < N_CLASSES; j++) s << std::setw(9) << cm[i][j];
        s << "\n";
    }
    const bool pass = agree >= count * 0.98;
    s << "\nRESULT: " << (pass ? "PASS" : "FAIL") << " (criterion: agreement >= 98 %)\n";

    std::cout << "\n" << s.str();
    std::ofstream(("csim_cnn_summary_" + tag.str() + ".txt").c_str()) << s.str();
    return pass ? 0 : 1;
}
