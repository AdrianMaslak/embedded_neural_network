open_project [file join [file dirname [info script]] build a2.xpr]
open_bd_design [get_files system.bd]
write_bd_layout -force -format svg -orientation portrait [file join [file dirname [info script]] out system_bd.svg]
write_bd_layout -force -format pdf -orientation landscape [file join [file dirname [info script]] out system_bd.pdf]
exit
