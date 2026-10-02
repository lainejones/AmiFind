# AmiFind

A file-search tool for AmigaOS 2.04 and newer (tested on 3.1 and 3.2). Recurses a volume, assign,
or directory and lists every file/drawer whose name matches a pattern.
Ships as a CLI (`AmiFind`) and a GadTools GUI (`AmiFindGUI`).

Built with amiga-gcc (`m68k-amigaos-gcc`) under WSL — pure NDK, no MUI/ReAction.

## Layout

    src/finder.c / finder.h   shared recursive search engine + matcher
    src/cli.c                 command-line front end
    src/gui.c                 GadTools GUI front end
    build.sh                  build both with amiga-gcc
    icons/                    the shipped icons: AmiFindGUI.info, and drawer.info
                              (goes beside the package drawer as AmiFind.info)
    out/                      compiled AmigaOS executables (not in git)

## Installing

Unpack the **.lha** wherever you keep tools (`LhA x AmiFind-1.1.lha Work:`), open the
AmiFind drawer and double-click **AmiFindGUI**. For the CLI version in any Shell, copy it
to `C:`:

    Copy AmiFind/AmiFind C:

From the **.zip** instead: a zip can't store AmigaDOS protection bits, so the programs
arrive without their `e` (executable) flag and won't run until you set it:

    Protect AmiFind/AmiFind +e
    Protect AmiFind/AmiFindGUI +e

## Build

From Windows, inside WSL:

    wsl -e bash -lc 'cd /mnt/c/projects/AmiFind && sh build.sh'

Outputs `out/AmiFind` and `out/AmiFindGUI` (AmigaOS m68k hunk executables) and
copies `icons/AmiFindGUI.info` beside the GUI. Copy them to your Amiga / emulator
and run.

The release package is an `AmiFind` drawer holding `AmiFind`, `AmiFindGUI`,
`AmiFindGUI.info` and `README.md`, with `icons/drawer.info` beside the drawer as
`AmiFind.info` (without it the unpacked drawer is invisible on Workbench). The
CLI `AmiFind` ships without an icon.

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

## Icons

The shipped icons are committed in `icons/`. Only the GUI has a program icon;
the CLI `AmiFind` ships without one. `icons/AmiFindGUI.info` is a `WBTOOL`
icon with an OS 3.5 colour image plus a planar fallback, built with a
self-contained Python generator (no png2icon / amitools needed) that lives in
the shared Amiga tools folder next to this project (`../tools`), not in this
repository:

    ../tools/makeicon_amifind.py   build the GUI icon
    ../tools/dumpicon.py           verify one (ASCII)

Regenerate it (the output is byte-identical to the shipped icon):

    wsl -e bash -lc 'cd /mnt/c/projects/AmiFind && python3 ../tools/makeicon_amifind.py icons/AmiFindGUI.info'

`icons/drawer.info` is the package drawer's icon, shipped beside the drawer as
`AmiFind.info`.

## Notes

* Matching uses `dos.library/ParsePatternNoCase` + `MatchPatternNoCase`, so it
  follows standard AmigaDOS semantics when wildcards are present.
* The GUI search is synchronous; while it runs the Stop button and close
  gadget stay responsive because the scan pumps Intuition messages between
  directory entries.
* Requires AmigaOS 2.04 or newer: the libraries are opened at v37 (asl,
  workbench and icon are optional), and the one V39 call (`ExAllEnd`) is
  skipped on an older dos.library. Tested on OS 3.1 and 3.2.
