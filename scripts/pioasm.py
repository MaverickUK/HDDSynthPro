# PlatformIO pre-build script: assemble every src/*.pio into a .pio.h header.
#
# The platform defines a PIO-to-header builder but never runs it, so this
# does it by hand. Headers go into the build directory (not src/), which is
# added to the include path, so `#include "ide_listen.pio.h"` just works.
# A header is rebuilt only when its .pio file is newer.
import os
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO)

pioasm_dir = env.PioPlatform().get_package_dir("tool-pioasm-rp2040-earlephilhower")  # noqa: F821
if not pioasm_dir:
    raise SystemExit("pioasm.py: the tool-pioasm-rp2040-earlephilhower package is missing")
pioasm = os.path.join(pioasm_dir, "pioasm")

src_dir = env.subst("$PROJECT_SRC_DIR")  # noqa: F821
out_dir = os.path.join(env.subst("$BUILD_DIR"), "pio_headers")  # noqa: F821
os.makedirs(out_dir, exist_ok=True)

for name in sorted(os.listdir(src_dir)):
    if not name.endswith(".pio"):
        continue
    source = os.path.join(src_dir, name)
    header = os.path.join(out_dir, name + ".h")
    if os.path.exists(header) and os.path.getmtime(header) >= os.path.getmtime(source):
        continue
    print(f"pioasm: {name} -> {os.path.relpath(header)}")
    subprocess.run([pioasm, "-o", "c-sdk", source, header], check=True)

env.Append(CPPPATH=[out_dir])  # noqa: F821
