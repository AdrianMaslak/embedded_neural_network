# Program the PYNQ-Z2 over JTAG and start the firmware.
#   xsdb firmware/run_on_board.tcl        (board in JTAG boot mode, USB connected)
# Afterwards close xsdb (it does not hold the COM port) and run pc/app.py.
set here [file dirname [file normalize [info script]]]
set root [file dirname $here]
set bit  [file join $root vivado out shape_system.bit]
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
puts "firmware running - open the COM port at 4000000 baud (pc/app.py info)"
