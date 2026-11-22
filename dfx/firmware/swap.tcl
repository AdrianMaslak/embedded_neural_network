# DFX: load a partial bitstream into the reconfigurable partition over JTAG while the
# firmware keeps running, and time it.
#   xsdb dfx/firmware/swap.tcl snn      (or cnn)
set here [file dirname [file normalize [info script]]]
set dfx  [file dirname $here]
set net  [lindex $argv 0]
if {$net ni {cnn snn}} { error "usage: xsdb swap.tcl cnn|snn" }
set bit  [file join $dfx vivado out ${net}_partial.bit]
connect
targets -set -nocase -filter {name =~ "*xc7z020*"}
set t0 [clock milliseconds]
fpga -partial -file $bit
set t1 [clock milliseconds]
puts [format "SWAP %s: partial bitstream %d bytes loaded over JTAG in %d ms" \
    [string toupper $net] [file size $bit] [expr {$t1 - $t0}]]
