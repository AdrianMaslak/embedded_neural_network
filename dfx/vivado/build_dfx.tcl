# Assignment 2 bonus - DFX build: one reconfigurable partition that holds the CNN or the SNN.
#
#   cd dfx\vivado
#   vivado -mode batch -source build_dfx.tcl        (path without spaces)
#
# Needs the HLS IPs from dfx\hls (run_hls_cnn.tcl, run_hls_snn.tcl) first.
#
# Block design "system" (static part)
#   PS7 M_AXI_GP0 -> interconnect -> rp_nn/s_axi_control_0   (CPU control, 0x4000_0000)
#   rp_nn/m_axi_gmem_0 -> interconnect -> PS7 S_AXI_HP0 -> DDR (both networks read their input from DDR)
# rp_nn is a DFX block design container with two variants:
#   rm_cnn.bd (cnn_rm IP)  and  rm_snn.bd (snn_rm IP)
# Two configurations are implemented: cfg_cnn (impl_1, parent) and cfg_snn (child run).
#
# Outputs in dfx\vivado\out:
#   full_cnn.bit, full_snn.bit         full bitstreams (static + one module)
#   cnn_partial.bit, snn_partial.bit   partial bitstreams for swapping at run time
#   shape_dfx.xsa                      hardware platform for the firmware (static + CNN)
#   reports: utilization (whole design, pblock, per module), timing, power, pr_verify

set here  [file dirname [file normalize [info script]]]
set dfx   [file dirname $here]
set out   [file join $here out]
set proj  [file join $here build]
set jobs  4
if {[info exists ::env(DFX_JOBS)]} { set jobs $::env(DFX_JOBS) }
file mkdir $out

proc step {msg} { puts "\n===== DFX STEP: $msg =====" }

# ---------------------------------------------------------------- project
if {[info exists ::env(PYNQZ2_BOARD_DIR)]} {
    set board_dir $::env(PYNQZ2_BOARD_DIR)
} else {
    set board_dir [file join [file normalize $::env(APPDATA)] \
        Xilinx/Vivado/2026.1/xhub/board_store/xilinx_board_store/XilinxBoardStore/Vivado/2026.1/boards/TUL/pynq-z2/A.0/1.0]
}
if {[file isdirectory $board_dir]} { set_param board.repoPaths [list $board_dir] }

step "create project"
create_project -force a2dfx $proj -part xc7z020clg400-1
if {[file isdirectory $board_dir]} { set_property board_part_repo_paths [list $board_dir] [current_project] }
set bp [lindex [get_board_parts -quiet *pynq-z2*] end]
if {$bp ne ""} { set_property board_part $bp [current_project] } else { puts "WARNING: PYNQ-Z2 board files not found, using part only" }
set_property PR_FLOW 1 [current_project]
set_property ip_repo_paths [list \
    [file join $dfx hls cnn_rm_prj sol1 impl ip] \
    [file join $dfx hls snn_rm_prj sol1 impl ip]] [current_project]
update_ip_catalog

# ---------------------------------------------------------------- RM 1: CNN
step "block design rm_cnn"
create_bd_design rm_cnn
create_bd_cell -type ip -vlnv sdu.dk:dfx:cnn_rm:1.0 nn
make_bd_intf_pins_external [get_bd_intf_pins nn/s_axi_control]
make_bd_intf_pins_external [get_bd_intf_pins nn/m_axi_gmem]
make_bd_pins_external [get_bd_pins nn/ap_clk]
make_bd_pins_external [get_bd_pins nn/ap_rst_n]
set_property CONFIG.ASSOCIATED_BUSIF {s_axi_control_0:m_axi_gmem_0} [get_bd_ports ap_clk_0]
set_property CONFIG.ASSOCIATED_RESET {ap_rst_n_0} [get_bd_ports ap_clk_0]
set_property CONFIG.FREQ_HZ 100000000 [get_bd_ports ap_clk_0]
assign_bd_address
validate_bd_design
save_bd_design

# ---------------------------------------------------------------- static design
step "block design system (static)"
create_bd_design system
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} [get_bd_cells ps]
set_property -dict [list CONFIG.PCW_USE_M_AXI_GP0 {1} CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100}] [get_bd_cells ps]

create_bd_cell -type container -reference rm_cnn rp_nn
set_property CONFIG.ENABLE_DFX {true} [get_bd_cells rp_nn]

# ---------------------------------------------------------------- RM 2: SNN (same boundary)
step "block design rm_snn (variant)"
create_bd_design -boundary_from_container [get_bd_cells /rp_nn] rm_snn
create_bd_cell -type ip -vlnv sdu.dk:dfx:snn_rm:1.0 nn
connect_bd_intf_net [get_bd_intf_ports s_axi_control_0] [get_bd_intf_pins nn/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins nn/m_axi_gmem] [get_bd_intf_ports m_axi_gmem_0]
connect_bd_net [get_bd_ports ap_clk_0]   [get_bd_pins nn/ap_clk]
connect_bd_net [get_bd_ports ap_rst_n_0] [get_bd_pins nn/ap_rst_n]
assign_bd_address
validate_bd_design
save_bd_design

step "connect static design"
current_bd_design system
set_property -dict [list CONFIG.LIST_SYNTH_BD {rm_cnn.bd:rm_snn.bd} CONFIG.LIST_SIM_BD {rm_cnn.bd:rm_snn.bd}] \
    [get_bd_cells rp_nn]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Master "/ps/M_AXI_GP0" Clk "Auto"} [get_bd_intf_pins rp_nn/s_axi_control_0]
# partition master -> SmartConnect -> HP0 (wired by hand: connection automation
# does not accept a DFX container pin as the master)
set rst [lindex [get_bd_cells rst_ps*] 0]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hp_sc
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] [get_bd_cells hp_sc]
connect_bd_intf_net [get_bd_intf_pins rp_nn/m_axi_gmem_0] [get_bd_intf_pins hp_sc/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins hp_sc/M00_AXI] [get_bd_intf_pins ps/S_AXI_HP0]
connect_bd_net [get_bd_pins ps/FCLK_CLK0] [get_bd_pins hp_sc/aclk]
connect_bd_net [get_bd_pins $rst/peripheral_aresetn] [get_bd_pins hp_sc/aresetn]
if {[llength [get_bd_nets -quiet -of_objects [get_bd_pins ps/S_AXI_HP0_ACLK]]] == 0} {
    connect_bd_net [get_bd_pins ps/FCLK_CLK0] [get_bd_pins ps/S_AXI_HP0_ACLK]
}
# clock / reset of the partition (automation may already have done this)
if {[llength [get_bd_nets -quiet -of_objects [get_bd_pins rp_nn/ap_clk_0]]] == 0} {
    connect_bd_net [get_bd_pins ps/FCLK_CLK0] [get_bd_pins rp_nn/ap_clk_0]
}
if {[llength [get_bd_nets -quiet -of_objects [get_bd_pins rp_nn/ap_rst_n_0]]] == 0} {
    connect_bd_net [get_bd_pins $rst/peripheral_aresetn] [get_bd_pins rp_nn/ap_rst_n_0]
}

assign_bd_address
# fixed control address, so the firmware does not depend on generated names
foreach seg [get_bd_addr_segs -of_objects [get_bd_addr_spaces ps/Data]] {
    if {[string match -nocase *rp_nn* $seg] || [string match -nocase *nn_Reg* $seg]} {
        set_property offset 0x40000000 $seg
        set_property range 64K $seg
    }
}
foreach s [get_bd_addr_segs -of_objects [get_bd_addr_spaces]] { puts "ADDR: $s [get_property offset $s] [get_property range $s]" }

validate_bd_design
save_bd_design
write_bd_tcl -force [file join $out system_bd.tcl]

step "generate + wrapper"
set bd [get_files system.bd]
generate_target all $bd
add_files -norecurse [make_wrapper -files $bd -top]
set_property top system_wrapper [current_fileset]
update_compile_order -fileset sources_1

# ---------------------------------------------------------------- floorplan
step "pblock constraints"
set xdc [file join $here pblock.xdc]
add_files -fileset constrs_1 -norecurse $xdc
set_property USED_IN {implementation} [get_files $xdc]

# ---------------------------------------------------------------- DFX configurations
step "DFX configurations"
puts "PARTITION DEFS: [get_partition_defs]"
puts "RECONFIG MODULES: [get_reconfig_modules]"
set rms [get_reconfig_modules]
set rm_cnn [lsearch -inline -nocase $rms *cnn*]
set rm_snn [lsearch -inline -nocase $rms *snn*]
if {$rm_cnn eq "" || $rm_snn eq ""} { error "reconfigurable modules not found: $rms" }
create_pr_configuration -name cfg_cnn -partitions [list system_i/rp_nn:$rm_cnn]
create_pr_configuration -name cfg_snn -partitions [list system_i/rp_nn:$rm_snn]
set_property PR_CONFIGURATION cfg_cnn [get_runs impl_1]
set flow [get_property FLOW [get_runs impl_1]]
create_run child_snn -parent_run impl_1 -flow $flow -pr_config cfg_snn

# ---------------------------------------------------------------- run
step "synthesis + implementation (both configurations)"
source [file join $here pblock_gen.tcl]
launch_runs impl_1 child_snn -to_step write_bitstream -jobs $jobs
wait_on_run impl_1
wait_on_run child_snn
foreach r {impl_1 child_snn} {
    puts "RUN $r: [get_property STATUS [get_runs $r]]  [get_property PROGRESS [get_runs $r]]"
    if {[get_property PROGRESS [get_runs $r]] != "100%"} { error "run $r failed" }
}

# ---------------------------------------------------------------- outputs
step "reports + bitstreams"
set d1 [get_property DIRECTORY [get_runs impl_1]]
set d2 [get_property DIRECTORY [get_runs child_snn]]
puts "impl_1 dir: [glob -nocomplain [file join $d1 *.bit]]"
puts "child dir:  [glob -nocomplain [file join $d2 *.bit]]"
foreach {dir tag} [list $d1 cnn $d2 snn] {
    foreach f [glob -nocomplain [file join $dir *.bit]] {
        if {[string match *partial* [file tail $f]]} {
            file copy -force $f [file join $out ${tag}_partial.bit]
        } else {
            file copy -force $f [file join $out full_${tag}.bit]
        }
    }
}
foreach f [glob -nocomplain [file join $out *.bit]] { puts "BIT: [file tail $f] [file size $f] bytes" }

# pr_verify between the two routed configurations
set dcp1 [lindex [glob -nocomplain [file join $d1 *_routed.dcp]] 0]
set dcp2 [lindex [glob -nocomplain [file join $d2 *_routed.dcp]] 0]
catch {pr_verify -initial $dcp1 -additional [list $dcp2] -file [file join $out pr_verify.rpt]} e
puts "pr_verify: $e"

open_run impl_1
catch {report_utilization -file [file join $out utilization_cnn.rpt]}
catch {report_utilization -pblocks [get_pblocks pblock_rp] -file [file join $out utilization_pblock.rpt]}
catch {report_utilization -hierarchical -hierarchical_depth 3 -file [file join $out utilization_hier_cnn.rpt]}
catch {report_timing_summary -file [file join $out timing_cnn.rpt]}
catch {report_power -hierarchical_depth 3 -file [file join $out power_cnn.rpt]}
write_hw_platform -fixed -include_bit -force [file join $out shape_dfx.xsa]
close_design
open_run child_snn
catch {report_utilization -file [file join $out utilization_snn.rpt]}
catch {report_utilization -hierarchical -hierarchical_depth 3 -file [file join $out utilization_hier_snn.rpt]}
catch {report_timing_summary -file [file join $out timing_snn.rpt]}
catch {report_power -hierarchical_depth 3 -file [file join $out power_snn.rpt]}
close_design
puts "DFX_BUILD_DONE"
