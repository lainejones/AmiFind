#!/bin/sh
# Build AmiFind (CLI + GUI + diagnostics) with amiga-gcc. Run inside WSL.
#   cd /mnt/c/projects/AmiFind && ./build.sh
set -e
export PATH=/opt/amiga/bin:$PATH

CC=m68k-amigaos-gcc
CFLAGS="-Os -msmall-code -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer"

mkdir -p out

echo "== AmiFind (CLI) =="
$CC $CFLAGS -s -o out/AmiFind        src/cli.c src/finder.c

echo "== AmiFindGUI (optimised) =="
$CC $CFLAGS -s -o out/AmiFindGUI     src/gui.c src/finder.c

# keep an unstripped + disassembly of the GUI for crash mapping (diagnostic)
$CC $CFLAGS -g -o out/AmiFindGUI.dbg src/gui.c src/finder.c
m68k-amigaos-objdump -dS out/AmiFindGUI.dbg > out/AmiFindGUI.dis 2>/dev/null || true

# Strip at LINK time with -s (above), NEVER the standalone m68k-amigaos-strip:
# the rebuilt binutils (amiga-2.46) strip CORRUPTS the hunk reloc table even for
# a single file -> wild-jump guru 8000000B at real timing. -s uses ld's correct
# stripping. (Older binutils only mangled multi-file strips; the 2026-07 rebuild
# is worse.) See the reference_amiga_toolchain_wsl memory.
ls -l out
echo "Done."
