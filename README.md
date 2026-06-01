# AmiFind

A file-search tool for AmigaOS 3.2 (and any 3.x). Recurses a volume, assign,
or directory and lists every file/drawer whose name matches a pattern.
Ships as a CLI (`AmiFind`) and a GadTools GUI (`AmiFindGUI`).

Built with amiga-gcc (`m68k-amigaos-gcc`) under WSL — pure NDK, no MUI/ReAction.

## Layout

    src/finder.c / finder.h   shared recursive search engine + matcher
    src/cli.c                 command-line front end
    src/gui.c                 GadTools GUI front end
    build.sh                  build both with amiga-gcc
    out/                      compiled AmigaOS executables

## Build

From Windows, inside WSL:

    wsl -e bash -lc 'cd /mnt/c/projects/AmiFind && sh build.sh'

Outputs `out/AmiFind` and `out/AmiFindGUI` (AmigaOS m68k hunk executables).
Copy them to your Amiga / emulator and run.

## CLI usage

    AmiFind PATTERN [PATH]

* `PATTERN` — plain text is matched as a **case-insensitive substring**.
  If it contains AmigaDOS wildcards (`#?  *  ?  [ ]  ~  |`) it is matched
  as a full AmigaDOS pattern instead.
* `PATH` — volume, assign or directory to search. Defaults to `SYS:`.
* Press **Ctrl-C** to abort.

Examples

    AmiFind startup-sequence            ; substring search under SYS:
    AmiFind #?.library LIBS:            ; all .library files in LIBS:
    AmiFind paint WORK:Graphics         ; substring under a directory
    AmiFind "#?.(info|prefs)" SYS:      ; AmigaDOS alternation

## GUI usage

Run `AmiFindGUI` (from Workbench or Shell). Enter a **Pattern** and a
**Search in** path (default `SYS:`), press **Search**. Results appear in the
listview; the status line shows the match count. **Stop** (or closing the
window) aborts a running search. Matching is identical to the CLI.

## Icons (reusable generator)

The `.info` icons are built with a self-contained Python generator — no
external tools (png2icon / amitools) required. It emits a classic OS3.x
`WBTOOL` `DiskObject` with a 2-bitplane `Image` in the 4-colour Workbench
palette, drawn procedurally. The generator is **shared across projects** in
`C:\projects\tools` (`/mnt/c/projects/tools`), not inside this project:

    ../tools/makeicon.py   build the icons     python3 ../tools/makeicon.py A.info B.info
    ../tools/dumpicon.py   verify one (ASCII)  python3 ../tools/dumpicon.py A.info

Regenerate both AmiFind icons:

    wsl -e bash -lc 'cd /mnt/c/projects/AmiFind && python3 ../tools/makeicon.py out/AmiFind.info out/AmiFindGUI.info'

To reuse for another project, edit the drawing section of `makeicon.py` (the
`rect`/`frame`/`disc`/`ring` calls) and `do_StackSize`/`WBTOOL` to taste; the
structure-packing below it is generic to any 3.x tool icon.

## Notes

* Matching uses `dos.library/ParsePatternNoCase` + `MatchPatternNoCase`, so it
  follows standard AmigaDOS semantics when wildcards are present.
* The GUI search is synchronous; while it runs the Stop button and close
  gadget stay responsive because the scan pumps Intuition messages between
  directory entries.
* Requires OS 3.x (libraries opened at v37+).
