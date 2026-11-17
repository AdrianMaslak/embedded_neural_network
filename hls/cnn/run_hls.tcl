# Vitis HLS: C-sim, synthesis and IP export of the CNN accelerator.
#   cd hls/cnn ; vitis-run --mode hls --tcl run_hls.tcl
# Optional env: HLS_COSIM=1 -> also C/RTL co-simulation, HLS_NO_CSIM=1 -> skip csim,
#               HLS_CSIM_ONLY=1 -> only C simulation (in project cnn_csim)
if {[info exists ::env(HLS_CSIM_ONLY)]} {
    open_project -reset cnn_csim
    set_top shape_cnn
    add_files shape_cnn.cpp
    add_files -tb shape_cnn_tb.cpp
    add_files -tb cnn_test_vectors.txt
    open_solution -reset sol1 -flow_target vivado
    set_part xc7z020clg400-1
    csim_design -O
    exit
}
open_project -reset cnn_prj
set_top shape_cnn
add_files shape_cnn.cpp
add_files -tb shape_cnn_tb.cpp
add_files -tb cnn_test_vectors.txt
open_solution -reset sol1 -flow_target vivado
set_part xc7z020clg400-1
create_clock -period 10 -name default
if {![info exists ::env(HLS_NO_CSIM)]} { csim_design }
csynth_design
if {[info exists ::env(HLS_COSIM)]} { cosim_design }
export_design -format ip_catalog -vendor sdu.dk -library hls -version 1.0 -display_name shape_cnn
exit
