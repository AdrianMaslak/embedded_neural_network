# DFX: program the PYNQ-Z2 with the full bitstream (static + CNN) and start the DFX firmware.
#   xsdb dfx/firmware/run_on_board.tcl [cnn|snn]      (default cnn; board in JTAG boot mode)
# Then swap modules with:  xsdb dfx/firmware/swap.tcl snn   /   ... swap.tcl cnn
set here [file dirname [file normalize [info script]]]
set dfx  [file dirname $here]
set net  cnn
if {[llength $argv] > 0} { set net [lindex $argv 0] }
set bit  [file join $dfx vivado out full_${net}.bit]
set elf  [file join $here ws a2_app build a2_app.elf]
set init [lindex [glob -nocomplain [file join $here ws a2_platform * ps7_init.tcl] \
                                   [file join $here ws a2_platform * * ps7_init.tcl] \
                                   [file join $here ws a2_platform * * * ps7_init.tcl]] 0]
if {$init eq ""} { error "ps7_init.tcl not found under ws/a2_platform - build the platform first" }

connect
targets -set -nocase -filter {name =~ "*APU*"}
rst -system
after 1000
targets -set -nocase -filter {name =~ "*xc7z020*"}
fpga -file $bit
targets -set -nocase -filter {name =~ "*APU*"}
source $init
ps7_init
ps7_post_config
targets -set -nocase -filter {name =~ "*Cortex-A9 MPCore #0*"}
dow $elf
con
puts "DFX firmware running with [string toupper $net] loaded - pc/app.py info"
