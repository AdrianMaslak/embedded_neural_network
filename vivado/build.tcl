# Assignment 2 - combined CNN + SNN hardware for the PYNQ-Z2
#
#   vivado -mode batch -source build.tcl          (run from a path without spaces)
#
# Block design
#   PS7 M_AXI_GP0 -> SmartConnect -> shape_cnn s_axi_control   (CPU control)
#                                 -> snn_mlp   s_axi_CTRL      (CPU control)
#                                 -> axi_bram_ctrl S_AXI       (CPU writes SNN features)
#   snn_mlp   m_axi_gmem -> (same SmartConnect) -> axi_bram_ctrl (SNN reads features)
#   shape_cnn m_axi_gmem -> SmartConnect -> PS7 S_AXI_HP0 -> DDR (CNN reads image)
# Outputs: vivado/out/{shape_system.xsa, *.bit, utilization/power/timing reports}

set here  [file dirname [file normalize [info script]]]
set root  [file dirname $here]
set out   [file join $here out]
set proj  [file join $here build]
file mkdir $out

# PYNQ-Z2 board files, installed through Vivado's board store (Tools > Vivado Store > Boards).
# Override with the environment variable PYNQZ2_BOARD_DIR if they live elsewhere.
if {[info exists ::env(PYNQZ2_BOARD_DIR)]} {
    set board_dir $::env(PYNQZ2_BOARD_DIR)
} else {
    set board_dir [file join [file normalize $::env(APPDATA)] \
        Xilinx/Vivado/2026.1/xhub/board_store/xilinx_board_store/XilinxBoardStore/Vivado/2026.1/boards/TUL/pynq-z2/A.0/1.0]
}
if {![file isdirectory $board_dir]} {
    error "PYNQ-Z2 board files not found in $board_dir - install them from the Vivado board store or set PYNQZ2_BOARD_DIR"
}
set_param board.repoPaths [list $board_dir]

create_project -force a2 $proj -part xc7z020clg400-1
set_property board_part [lindex [get_board_parts -quiet *pynq-z2*] end] [current_project]
set_property ip_repo_paths [list \
    [file join $root hls cnn cnn_prj sol1 impl ip] \
    [file join $root hls snn snn_prj sol1 impl ip]] [current_project]
update_ip_catalog

create_bd_design system

# ---- Zynq PS ----
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} [get_bd_cells ps]
set_property -dict [list CONFIG.PCW_USE_M_AXI_GP0 {1} CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100}] [get_bd_cells ps]

# ---- accelerators + feature BRAM ----
create_bd_cell -type ip -vlnv sdu.dk:hls:shape_cnn:1.0 shape_cnn_0
create_bd_cell -type ip -vlnv sdu.dk:hls:snn_mlp:1.0 snn_mlp_0
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 axi_bram_ctrl_0
set_property -dict [list CONFIG.DATA_WIDTH {32} CONFIG.SINGLE_PORT_BRAM {1} CONFIG.ECC_TYPE {0}] \
    [get_bd_cells axi_bram_ctrl_0]

# CPU -> control ports and BRAM
foreach pin {shape_cnn_0/s_axi_control snn_mlp_0/s_axi_CTRL axi_bram_ctrl_0/S_AXI} {
    apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
        -config {Master "/ps/M_AXI_GP0" Clk "Auto"} [get_bd_intf_pins $pin]
}
apply_bd_automation -rule xilinx.com:bd_rule:bram_cntlr -config {BRAM "Auto"} \
    [get_bd_intf_pins axi_bram_ctrl_0/BRAM_PORTA]
# SNN master -> BRAM controller. The BRAM controller has a single slave port,
# already driven by the CPU interconnect, so the SNN joins that interconnect as
# a second slave input (connection automation leaves it on a dead-end
# interconnect instead, and the SNN then waits forever for its reads).
set periph [get_bd_cells -of_objects [get_bd_intf_pins -of_objects \
    [get_bd_intf_nets -of_objects [get_bd_intf_pins axi_bram_ctrl_0/S_AXI]] -filter {MODE == Master}]]
set_property CONFIG.NUM_SI {2} $periph
connect_bd_intf_net [get_bd_intf_pins snn_mlp_0/m_axi_gmem] [get_bd_intf_pins $periph/S01_AXI]
connect_bd_net [get_bd_pins ps/FCLK_CLK0] [get_bd_pins $periph/S01_ACLK]
connect_bd_net [get_bd_pins [get_bd_cells rst_ps_*]/peripheral_aresetn] [get_bd_pins $periph/S01_ARESETN]
# CNN master -> DDR through HP0
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Master "/shape_cnn_0/m_axi_gmem" Slave "/ps/S_AXI_HP0" Clk "Auto"} \
    [get_bd_intf_pins ps/S_AXI_HP0]

# ---- address map ----
assign_bd_address
# The feature BRAM must sit at the same address for the CPU and for the SNN,
# because the firmware passes the CPU-side address to the SNN.
# (0x40000000 is taken by the CNN control registers.)
set bram_segs [get_bd_addr_segs -quiet {/ps/Data/SEG_axi_bram_ctrl_0_* /snn_mlp_0/Data_m_axi_gmem/SEG_axi_bram_ctrl_0_*}]
if {[llength $bram_segs] != 2} { error "feature BRAM not mapped for both CPU and SNN: $bram_segs" }
foreach seg $bram_segs {
    set_property offset 0x40020000 $seg
    set_property range 8K $seg
}
foreach s [get_bd_addr_segs -of_objects [get_bd_addr_spaces]] { puts "ADDR: $s [get_property offset $s] [get_property range $s]" }

validate_bd_design
save_bd_design
set bd [get_files system.bd]
generate_target all $bd
add_files -norecurse [make_wrapper -files $bd -top]
set_property top system_wrapper [current_fileset]
update_compile_order -fileset sources_1
write_bd_tcl -force [file join $out system_bd.tcl]

# ---- implementation ----
launch_runs impl_1 -to_step write_bitstream -jobs 2
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} { error "implementation failed" }

open_run impl_1
catch {report_utilization -file [file join $out utilization.rpt]}
catch {report_utilization -hierarchical -hierarchical_depth 2 -file [file join $out utilization_hier.rpt]}
catch {report_power -file [file join $out power.rpt] -hierarchical_depth 3}
catch {report_timing_summary -file [file join $out timing.rpt]}
write_hw_platform -fixed -include_bit -force [file join $out shape_system.xsa]
file copy -force [glob [file join $proj a2.runs impl_1 *.bit]] [file join $out shape_system.bit]
puts "BUILD_DONE"
