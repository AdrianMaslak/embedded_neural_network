# Vitis HLS: C-sim, synthesis and IP export of the SNN accelerator.
#   cd hls/snn ; vitis-run --mode hls --tcl run_hls.tcl
# Optional: set env HLS_COSIM=1 to also run C/RTL co-simulation.
open_project -reset snn_prj
set_top snn_mlp
add_files snn_mlp.cpp
add_files -tb snn_mlp_tb.cpp
add_files -tb snn_test_vectors.txt
open_solution -reset sol1 -flow_target vivado
set_part xc7z020clg400-1
create_clock -period 10 -name default
csim_design
csynth_design
if {[info exists ::env(HLS_COSIM)]} { cosim_design }
export_design -format ip_catalog -vendor sdu.dk -library hls -version 1.0 -display_name snn_mlp
exit
