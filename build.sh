#!/bin/sh
# Build AmiFind (CLI + GUI + diagnostics) with amiga-gcc. Run inside WSL.
#   cd /mnt/c/projects/AmiFind && ./build.sh
set -e
export PATH=/opt/amiga/bin:$PATH

CC=m68k-amigaos-gcc
CFLAGS="-Os -msmall-code -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer"

mkdir -p out

echo "== AmiFind (CLI) =="
$CC $CFLAGS    -o out/AmiFind        src/cli.c src/finder.c

echo "== AmiFindGUI (optimised) =="
$CC $CFLAGS    -o out/AmiFindGUI     src/gui.c src/finder.c

# keep an unstripped + disassembly of the GUI for crash mapping (diagnostic)
$CC $CFLAGS -g -o out/AmiFindGUI.dbg src/gui.c src/finder.c
m68k-amigaos-objdump -dS out/AmiFindGUI.dbg > out/AmiFindGUI.dis 2>/dev/null || true

# strip each file in its OWN invocation: m68k-amigaos-strip corrupts the hunk
# reloc table of every file after the first when given several at once (libnix
# startup jumps to garbage, guru #8000000x pre-main). See amiga-gcc-strip-multifile-bug.
for f in out/AmiFind out/AmiFindGUI; do
    m68k-amigaos-strip "$f" || true
done
ls -l out
echo "Done."
