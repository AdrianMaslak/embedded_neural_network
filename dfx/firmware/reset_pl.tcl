# DFX: after loading a partial bitstream, pulse the PL reset (FCLK_RESET0_N via SLCR
# FPGA_RST_CTRL). This resets the AXI interconnects and the new module, so no state
# left over from the reconfiguration (the region's outputs toggle while it is being
# written) blocks the next AXI access. Used by swap.tcl; can also be run on its own:
#   xsdb dfx/firmware/reset_pl.tcl
connect
targets -set -nocase -filter {name =~ "*APU*"}
mwr -force 0xF8000008 0xDF0D
mwr -force 0xF8000240 0x1
after 2
mwr -force 0xF8000240 0x0
puts "PL reset pulsed (FPGA_RST_CTRL)"
