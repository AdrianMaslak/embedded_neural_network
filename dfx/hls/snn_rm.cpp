// DFX reconfigurable module 2: SNN.
// Wraps the unchanged accelerator in hls/snn/snn_mlp.cpp with the same ports
// and register map as cnn_rm (cnn_rm.cpp). In the DFX design the SNN reads
// its 512 features from DDR (through S_AXI_HP0, like the CNN) instead of from
// the AXI BRAM, so both modules share one m_axi connection.
// net_id = 2 marks the SNN.
#include "../../hls/snn/snn_mlp.cpp"

void snn_rm(volatile data_t *in, int *pred, int logits[NUM_CLASSES], int *net_id)
{
#pragma HLS INTERFACE m_axi port=in offset=slave bundle=gmem depth=512 \
    max_read_burst_length=256 max_write_burst_length=16 \
    num_read_outstanding=16 num_write_outstanding=16 latency=64
#pragma HLS INTERFACE s_axilite port=in bundle=control
#pragma HLS INTERFACE s_axilite port=pred bundle=control
#pragma HLS INTERFACE s_axilite port=logits bundle=control
#pragma HLS INTERFACE s_axilite port=net_id bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    ap_uint<8> p;
    acc_t l[NUM_CLASSES];
    snn_mlp(in, &p, l);
    *pred = (int)p;
    for (int i = 0; i < NUM_CLASSES; i++) {
#pragma HLS UNROLL
        logits[i] = (int)l[i];
    }
    *net_id = 2;
}
