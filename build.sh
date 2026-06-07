#!/bin/sh
# Build AmiFind (CLI + GUI + diagnostics) with amiga-gcc. Run inside WSL.
#   cd /mnt/c/projects/AmiFind && ./build.sh
set -e
export PATH=/opt/amiga/bin:$PATH

CC=m68k-amigaos-gcc
CFLAGS="-O2 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer"
CFLAGS_O0="-O0 -noixemul -Wall -Wno-pointer-sign"

mkdir -p out

echo "== AmiFind (CLI) =="
$CC $CFLAGS    -o out/AmiFind        src/cli.c src/finder.c

echo "== AmiFindGUI (optimised) =="
$CC $CFLAGS    -o out/AmiFindGUI     src/gui.c src/finder.c

echo "== AmiFindGUI_O0 (no optimiser - codegen control) =="
$CC $CFLAGS_O0 -o out/AmiFindGUI_O0  src/gui.c src/finder.c

echo "== AmiFindMin (minimal GadTools window) =="
$CC $CFLAGS    -o out/AmiFindMin     src/mintest.c

# keep an unstripped + disassembly of the optimised GUI for crash mapping
$CC $CFLAGS -g -o out/AmiFindGUI.dbg src/gui.c src/finder.c
m68k-amigaos-objdump -dS out/AmiFindGUI.dbg > out/AmiFindGUI.dis 2>/dev/null || true

# strip each file in its OWN invocation: m68k-amigaos-strip corrupts the hunk
# reloc table of every file after the first when given several at once (libnix
# startup jumps to garbage, guru #8000000x pre-main). See amiga-gcc-strip-multifile-bug.
for f in out/AmiFind out/AmiFindGUI out/AmiFindGUI_O0 out/AmiFindMin; do
    m68k-amigaos-strip "$f" || true
done
ls -l out
echo "Done."
