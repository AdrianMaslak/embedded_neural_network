"""
Build the DFX firmware with the Vitis Python API (Vitis 2026.1).

    vitis -s dfx/firmware/build_sw.py        (run from the assignment2 root,
                                              on a path without spaces)
Creates   dfx/firmware/ws/a2_platform   (standalone BSP from vivado/out/shape_system.xsa)
          dfx/firmware/ws/a2_app        (main.c + preprocess.c)
Result    dfx/firmware/ws/a2_app/build/a2_app.elf
"""
import os
import shutil

import vitis

here = os.path.dirname(os.path.abspath(__file__))
dfx = os.path.dirname(here)
ws = os.path.join(here, "ws")
xsa = os.path.join(dfx, "vivado", "out", "shape_dfx.xsa")
srcs = ["main.c", "preprocess.c", "preprocess.h", "protocol.h"]

def _force_remove(func, path, _exc):
    # Vitis marks some generated headers read-only; clear the flag and retry.
    os.chmod(path, 0o666)
    func(path)


if os.path.isdir(ws):
    shutil.rmtree(ws, onexc=_force_remove)
os.makedirs(ws)

client = vitis.create_client()
client.set_workspace(path=ws)

platform = client.create_platform_component(
    name="a2_platform", hw_design=xsa, os="standalone",
    cpu="ps7_cortexa9_0", domain_name="standalone_ps7_cortexa9_0")
platform.build()
xpfm = os.path.join(ws, "a2_platform", "export", "a2_platform", "a2_platform.xpfm")

app = client.create_app_component(
    name="a2_app", platform=xpfm, domain="standalone_ps7_cortexa9_0",
    template="empty_application")
app.import_files(from_loc=here, files=srcs, dest_dir_in_cmp="src")
app.build()

elf = os.path.join(ws, "a2_app", "build", "a2_app.elf")
print("ELF:", elf, os.path.exists(elf))
vitis.dispose()
