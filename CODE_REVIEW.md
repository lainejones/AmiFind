# AmiFind — Code Review

Scope: `src/cli.c`, `src/finder.c`, `src/finder.h`, `src/gui.c`, `src/mintest.c`, `src/crashtest.c`, `src/trapcatch.c`, `build.sh`, `README.md`. All read in full. No files modified.

The fundamentals are solid: per-level `AllocDosObject(DOS_FIB)` keeps the FIB off the stack and gives each recursion level its own ExNext cursor (correct — a shared fib would break ExNext state); every `Lock` has a matching `UnLock`; every `FreeDosObject` is paired; `closeWin()` ordering (ClearMenuStrip → FreeMenus → CloseWindow → FreeGadgets → FreeVisualInfo → UnlockPubScreen) is textbook-correct; library opens/closes balance in all paths; `__stack = 60000` covers the recursion. The issues below are mostly edge cases — but several are real.

---

## 1. ISSUES

### High

**H1. `AddPart` failure path leaves `c->path` unrestored — corrupts the rest of the directory scan.**
`finder.c:107-108`: when `AddPart` fails (path would exceed 520 bytes), the code does `continue`, which skips the restore at `finder.c:133` (`c->path[plen] = '\0'`). The AddPart autodoc does not guarantee the buffer is untouched on failure — it may have already appended the separator or a partial name. From that point on, **every remaining entry in that directory** is appended to a corrupted base path: wrong paths reported, wrong dirs locked and recursed into (or Lock failures that silently skip whole subtrees). Triggered by any tree deeper than ~519 chars — exactly the deep trees this tool exists to search. Fix is one line: restore `c->path[plen] = '\0'` before the `continue` (it's cheap and correct whether or not AddPart mutated the buffer).

**H2. Failed un-iconify strands the process in an unkillable infinite `Wait`.**
`gui.c:483-491` (`doUniconify`): `RemoveAppIcon` is called first, then `buildWindow(g)`. If `buildWindow` fails (pub screen lock or `OpenWindowTags` fails — e.g. low memory, which is precisely when you'd be iconified), you now have **no window and no AppIcon**, but `g->appPort` still exists. `eventLoop` (`gui.c:499-505`) computes `winSig = 0`, `appSig != 0`, so the `if (!winSig && !appSig) break;` escape never fires and the task sits in `Wait()` on a port nothing will ever signal. No way for the user to quit except a task killer. Same failure class at `gui.c:479`: if `AddAppIconA` fails *and* the retry `buildWindow(g)` also fails, identical zombie. Fix: if reopening fails, fall back to `done` (clean exit) rather than re-entering the loop.

**H3. `relayout()` partial failure leaves stale/dangerous gadget state.**
`gui.c:371-388`: on `IDCMP_NEWSIZE`, the old gadgets are removed and freed, then `makeGadgets()` runs. If `makeGadgets` fails (`CreateContext`/`CreateGadget` returns NULL under low memory):
- `g->glist` holds a **partial, never-added** gadget list. The next `relayout()` (or `saveFields()` via the next iconify) calls `RemoveGList(g->win, g->glist, -1)` on gadgets that were never added to the window — undefined behavior.
- `g->gPattern` / `g->gPath` / `g->gStatus` still point at the **freed** previous-generation gadgets. The next Search click runs `doSearch()` → `gadgetString(g->gPattern)` (`gui.c:210-214`) → read through freed memory; `setStatus` → `GT_SetGadgetAttrs` on a freed gadget. Low-memory only, but it's a crash, and low memory is the normal operating condition on target hardware. Also note `makeGadgets` only checks the *last* `gad` (`gui.c:357`), which is fine for detecting failure (CreateGadget NULL-propagates) but doesn't help the caller recover.

### Medium

**M1. Stale `IAddress` dereference after relayout frees gadgets mid-drain.**
`gui.c:510-521` + `gui.c:542-543`: the inner `while (GT_GetIMsg(...))` loop handles `IDCMP_NEWSIZE` by calling `relayout()` — which frees all gadgets — and then **continues draining the queue**. Any already-queued `IDCMP_GADGETUP` from before the resize carries an `IAddress` pointing at a now-freed gadget; `gad->GadgetID` at `gui.c:521` reads freed memory (and may match `GAD_SEARCH` by luck, starting a search). Low probability (needs click+resize racing into the same queue) but real. Either break out of the drain loop after `relayout()`, or drain fully before relaying out. (Reading `gad->GadgetID` *after* `GT_ReplyIMsg` at `gui.c:514`/`gui.c:169` is otherwise fine — replying recycles the message, not the gadget — but it makes this failure mode easier to hit.)

**M2. The walker follows links — can escape the search root and inflate the scan.**
`finder.c:110-123`: any entry with `fib_DirEntryType > 0` is recursed into, which includes `ST_SOFTLINK` (3) and `ST_LINKDIR` (4). `Lock()` resolves soft links, so a link to `SYS:` inside the searched tree silently re-scans the whole volume (duplicated results), and a link loop recurses until `AddPart` overflows the 520-byte buffer (bounded, thanks to the buffer — not an infinite loop, but combined with H1 it corrupts the scan). Consider skipping `ST_SOFTLINK`/`ST_LINKDIR` (or at least soft links).

**M3. `IDCMP_NEWSIZE` is silently discarded during a running search.**
`gui.c:163-189` (`pumpMessages`): the default case swallows `IDCMP_NEWSIZE` (and `IDCMP_MENUPICK`). If the user resizes the window mid-search, no relayout ever happens — the message is consumed and Intuition won't resend it — so the gadgets stay sized for the old window geometry permanently (listview clipped or floating in empty space). Cheap fix: set a `g->resizePending` flag in `pumpMessages` and call `relayout()` after `searchTree` returns in `doSearch`.

**M4. trapcatch handler is wrong for group-0 exceptions; the comment overstates its safety.**
`trapcatch.c:58-71`: the stub assumes SR at 4(sp)/PC at 6(sp) after dropping the trap number. True for group-1/2 (illegal instruction, TRAP#n, divide-by-zero). **False for 68000 bus/address errors** (group 0 pushes 8 extra bytes — function code, access address, IR — *below* SR), so the stub patches a data word and `RTE`s a malformed frame → double guru. On 68020+, bus-fault frames (format $A/$B) hold pending-fault internal state; rewriting the PC and RTE-ing will attempt to complete/rerun the stacked fault — not a clean redirect. Since address errors are among the most common real crashes you'd want to catch on a stock 68000/68EC020 A1200, this limits the tool's usefulness exactly where intended (the file-header comment at `trapcatch.c:9` claims plain-68EC020 support). It's diagnostic scaffolding, so: at minimum check the exception number in the stub and only redirect for group-1/2, letting group-0 fall through to the default guru. Also note `crashExit` (`trapcatch.c:37`) calls `Open()`/`Write()` with whatever the crashed task's USP was — if the crash was a stack overflow, the log write itself may fault.

### Low

**L1. Substring patterns silently truncated at 139 chars.**
`finder.h:18` (`lower[140]`) vs. `GTST_MaxChars, 200` (`gui.c:318`, `gui.c:326`) and unlimited ReadArgs input in the CLI. `matcherInit` (`finder.c:58`) lowercases into the 140-byte buffer with truncation — a 150-char non-wildcard pattern matches on its first 139 chars with no error. (Wildcard path is fine: `parsed[512]` covers 2·len+2 for 200-char patterns, and over-long ones fail loudly.) Either size `lower` to match `MaxChars`+1 or reject over-long plain patterns in `matcherInit`.

**L2. CLI leaves SIGBREAKF_CTRL_C pending.**
`cli.c:28`: `SetSignal(0L, 0L)` reads but never clears the break signal. Harmless here (the program exits immediately after the abort), but the conventional idiom is `SetSignal(0L, SIGBREAKF_CTRL_C)` once you've decided to act on it.

**L3. Unbounded result list + silent OOM undercount.**
`gui.c:121-130`: a broad search (empty pattern over a big volume) allocates a node per match with no cap; on a 2 MB machine you'll exhaust memory mid-search. `addResult` then silently drops nodes while `st.matches` keeps counting, so the status line ("N match(es)") disagrees with the listview contents. Consider a cap with a "results truncated" status, or at least set a flag when `AllocVec` fails.

**L4. `closeWin` leaves dangling gadget pointers.**
`gui.c:453-461`: only `g->glist` is NULLed; `gPattern…gList` keep pointing at freed gadgets while iconified. Currently nothing reachable touches them in that state (setStatus is guarded only by `gStatus != NULL`, `gui.c:136`, not by `g->win`), but it's a loaded latent bug — one future `setStatus` call in an iconified code path is a crash. Null them in `closeWin` and add a `g->win` guard to `setStatus`.

**L5. `searchTree` can return 0 for a failed Lock.**
`finder.c:169-170`: returns `IoErr()` after a failed `Lock`. If a handler fails without setting IoErr (rare but possible with flaky network/CD handlers), the function returns 0 = "success" with zero stats and the GUI happily reports "0 match(es)". Cheap hardening: `LONG e = IoErr(); return e ? e : ERROR_OBJECT_NOT_FOUND;`.

**L6. Listview height can go non-positive with large screen fonts.**
`gui.c:349-356`: `ng_Height = cb - y` is unclamped (width gets clamped at `gui.c:344`, height doesn't). At `WA_MinHeight` 110 with a tall Workbench font, `cb - y` can go ≤ 0 → CreateGadget failure → the H3 stale-pointer cascade. Clamp it like the width.

**Nits:** Enter in the Find string gadget doesn't trigger a search (GADGETUP for `GAD_PATTERN` is ignored, `gui.c:520-526`) — standard GadTools UX would start one. `guiPoll` (`gui.c:196-202`) re-sets the status to the same `"Searching..."` string every 1024 entries — pure render overhead/flicker; the comment suggests a counter was intended (see size note S3 for a free fix). Missing-library failure from Workbench exits with rc 20 and zero user feedback (`gui.c:644-648`) — an `EasyRequest` via Intuition (if it opened) would help. `crashtest.c:12` / the stress harness write to `SYS:AFtest/...` which silently no-ops if the drawer doesn't exist. ExNext termination doesn't distinguish `ERROR_NO_MORE_ENTRIES` from a mid-scan I/O error (`finder.c:100`) — silent partial results on a failing disk.

---

## 2. SIZE REDUCTION

First, a discrepancy: **`build.sh` actually uses `-O2` + post-link `m68k-amigaos-strip`** (`build.sh:8`, `build.sh:32-34`), not `-Os`/`-s`. So the cheapest win is still on the table:

**S1. `-O2` → `-Os`.** On m68k, `-Os` vs `-O2` typically shaves 5–15% of generated code; for binaries this size, on the order of 0.5–1.5 KB each. Nothing here is performance-critical enough to care (the walk is I/O-bound; `substr_ci` is the only hot loop and it's trivial). `-fomit-frame-pointer` is already set — keep it.

**S2. You've already won the big one — keep stdio out.** Neither binary calls `printf`/`sprintf`/`fopen`; the CLI uses `Printf`/`PrintFault` and the GUI hand-rolls its itoa precisely to avoid it (the comment at `gui.c:198-199` shows the awareness). Linking libnix stdio would cost roughly 10–20 KB; the current dos.library-only approach is the single biggest size decision in the project and it's already correct. The only libc that links now is the small string set (`strlen/strcpy/strncpy/strcat/strncmp/memset`) — a few hundred bytes total, not worth chasing.

**S3. Replace the hand-rolled itoa with `RawDoFmt`.** `gui.c:269-279` builds the match count manually with a reverse-digit loop plus `strcpy`/`strcat`. `RawDoFmt("%lu match(es)", ...)` into `buf` via exec (with the classic 2-instruction stuff-routine, or the tiny PutChProc) is an exec library call — zero linked code — and saves ~100–150 bytes while also giving `guiPoll` a free live counter ("Searching... 12345 scanned"), fixing the nit above. Same trick is already implicitly used by the CLI's `Printf`.

**S4. Merge the two binaries — the largest realistic win.** `finder.c` (~1.5–2 KB of code), the libnix startup (~2–4 KB), and the string runtime are duplicated across `AmiFind` and `AmiFindGUI`. One binary that branches on `argc == 0` (WBStartup) → GUI, else CLI, ships **one** copy: roughly 6–10 KB off the distribution total, plus one icon instead of two. The CLI path adds only its ReadArgs block to the GUI binary (~0.5 KB). This is the only change with a multi-KB payoff.

**S5. `-msmall-code` and `-fbaserel`.** Both binaries are far under the 32 KB pc-relative range. `-msmall-code` turns absolute `jsr`s into pc-relative ones (2 bytes/call) and, more importantly, **shrinks the hunk reloc table** — each eliminated 32-bit reloc saves 4 bytes of file. `-fbaserel` does the same for data references (6-byte absolute → 4-byte d16(a4), relocs gone) and makes the binaries pure/residentable as a bonus. Combined estimate: 3–8% of file size. Caveats: `trapcatch.c`'s inline asm uses absolute `lea _g_crashType` (`trapcatch.c:62-67`) — incompatible with `-fbaserel` — but trapcatch is never linked into the release binaries (see S7), so only the diagnostic builds need to stay non-baserel. The GUI's callbacks are only invoked from its own code (no interrupt/foreign-task callers), so no `__saveds` headaches.

**S6. `-ffunction-sections -fdata-sections -Wl,--gc-sections`.** Supported by bebbo's binutils. Honest assessment: minimal payoff here — there's almost no dead code in the release translation units (every function in `finder.c`/`cli.c`/`gui.c` is reachable; the `AFTRACE`/`AFCATCH`/`AFSTRESS` blocks are preprocessed out, and `g_tr`/`TR()` fold to nothing at -O2 since `g_tr` is a never-written static — verified: `gui.c:48-57`, the `if (g_tr) Close(g_tr)` at `gui.c:650` const-propagates away). Expect 0–300 bytes. Worth adding as insurance against future dead code, not as a fix.

**S7. Test scaffolding: clean, but trim the shipped artifacts.** `mintest.c`, `crashtest.c`, `trapcatch.c` do **not** leak into the main binaries — `build.sh` never compiles trapcatch.c at all and never defines the AF* macros (note: flipping on `AFCATCH`/`AFSTRESS` in CFLAGS today would be an immediate link error since trapcatch.c isn't on the gui link line — `build.sh:17` — worth a comment). What *does* bloat the output drop: `build.sh:20` ships `AmiFindGUI_O0` (an -O0 build, ~1.5–2× the optimized size) and `build.sh:26-27` ships `AmiFindGUI.dbg` + a full `.dis` disassembly into `out/` alongside the release binaries. Split build.sh into release vs. diagnostic targets so `out/` contains only `AmiFind`, `AmiFindGUI`, and icons.

**S8. Stripping: use `-s` on the link line.** The per-file strip loop (`build.sh:32-34`) exists to dodge the documented multi-file strip corruption bug — fair — but passing `-s` to the gcc driver strips at link time, one binary per invocation, which sidesteps the bug by construction and removes the workaround loop entirely. Size result is identical; it's a robustness simplification.

**S9. Static buffers: non-issue, leave them.** `Matcher` (652 B), `Ctx` (~560 B), and `Gui` (~600 B) are all stack automatics, not statics — they cost stack (amply covered by `__stack = 60000`) and zero file bytes. There is no BSS/data-hunk bloat to fix; even if they were static, zeroed BSS hunks store only a size word in the hunk file. Don't shrink `parsed[512]` — it's exactly right for 200-char patterns (ParsePattern needs 2·len+2).

**S10. Diminishing returns / not recommended:** `-nostartfiles` with a custom entry could get the CLI down to ~3–4 KB total, but you'd lose libnix's `__stack` auto-stack-swap (you'd have to hand-roll `StackSwap`, and the 60 KB stack is load-bearing for the recursion) plus WBStartup handling in the GUI. Not worth it. `-mcrt=nix20` is just the modern spelling of `-noixemul` — no size change; `nix13` saves a trivial amount but targets 1.3 semantics you don't need.

**Rough expected totals** (typical bebbo libnix sizes for this code: CLI ~8–10 KB, GUI ~16–20 KB): S1+S3+S5 ≈ 1–2.5 KB off each binary; S4 ≈ 6–10 KB off the distribution; S7 removes ~30–80 KB of diagnostic files from `out/`.
