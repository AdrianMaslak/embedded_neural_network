// DFX reconfigurable module 1: CNN.
// Wraps the unchanged accelerator in hls/cnn/shape_cnn.cpp so that it has
// exactly the same ports and AXI-Lite register map as snn_rm (snn_rm.cpp):
//   s_axi_control: in (64-bit address), pred, logits[4], net_id   (+ ap_ctrl)
//   m_axi_gmem:    32-bit data, 64-bit address, same burst settings
// net_id lets the firmware see which module is currently loaded (1 = CNN).
#include "../../hls/cnn/shape_cnn.cpp"

void cnn_rm(const data_t *in, int *pred, int logits[N_CLASSES], int *net_id)
{
#pragma HLS INTERFACE m_axi port=in offset=slave bundle=gmem depth=4800 \
    max_read_burst_length=256 max_write_burst_length=16 \
    num_read_outstanding=16 num_write_outstanding=16 latency=64
#pragma HLS INTERFACE s_axilite port=in bundle=control
#pragma HLS INTERFACE s_axilite port=pred bundle=control
#pragma HLS INTERFACE s_axilite port=logits bundle=control
#pragma HLS INTERFACE s_axilite port=net_id bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    int p, l[N_CLASSES];
    shape_cnn(in, &p, l);
    *pred = p;
    for (int i = 0; i < N_CLASSES; i++) {
#pragma HLS UNROLL
        logits[i] = l[i];
    }
    *net_id = 1;
}
