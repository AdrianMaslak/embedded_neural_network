# Vitis HLS for the DFX module cnn_rm (synthesis + IP export, no C-sim:
# the wrapped accelerator is the one verified in hls/cnn/).
#   cd dfx\hls ; vitis-run --mode hls --tcl run_hls_cnn.tcl
open_project -reset cnn_rm_prj
set_top cnn_rm
add_files cnn_rm.cpp
open_solution -reset sol1 -flow_target vivado
set_part xc7z020clg400-1
create_clock -period 10 -name default
csynth_design
export_design -format ip_catalog -vendor sdu.dk -library dfx -version 1.0 -display_name cnn_rm
exit
