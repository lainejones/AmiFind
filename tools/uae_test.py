#!/usr/bin/env python3
"""
Harness to reproduce the AmiFindGUI crash inside the WinUAE 'CNETtest' install.

DH0: is the host directory /mnt/c/Amiga/a1200HD-2. This script:
  setup   - back up S/Startup-Sequence + S/User-Startup, disable the CNet BBS
            autostart, and insert an auto-run of SYS:AFtest/RUNME after LoadWB.
  use X   - copy out/X onto SYS:AFtest/RUNME (the binary the boot will launch).
  restore - put the original boot files back and remove AFtest.

Usage (run under WSL):
  python3 tools/uae_test.py setup
  python3 tools/uae_test.py use AmiFindGUI
  python3 tools/uae_test.py restore
"""
import os, sys, shutil

DH0 = "/mnt/c/Amiga/a1200HD-2"
OUT = os.path.join(os.path.dirname(__file__), "..", "out")
SS  = os.path.join(DH0, "S", "Startup-Sequence")
US  = os.path.join(DH0, "S", "User-Startup")
AFT = os.path.join(DH0, "AFtest")
MARK = ";AFTEST"

def read(p):  return open(p, "r", newline="").read()
def write(p, s):                       # always write LF only (Amiga style)
    with open(p, "w", newline="") as f: f.write(s.replace("\r\n", "\n"))

def setup():
    for p in (SS, US):
        if not os.path.exists(p + ".afbak"):
            shutil.copyfile(p, p + ".afbak")
    os.makedirs(AFT, exist_ok=True)

    # disable BBS / server autostart so we land on a clean Workbench
    us = read(US).split("\n")
    out = []
    for ln in us:
        s = ln.strip().lower()
        if s.startswith("run") and ("cnet:control" in s or "hsserver" in s):
            out.append(MARK + "-off " + ln)
        else:
            out.append(ln)
    write(US, "\n".join(out))

    # insert auto-run of RUNME right before the final EndCLI
    ss = read(SS).split("\n")
    ss = [l for l in ss if MARK not in l]          # idempotent
    # args are harmless to the GUI (ignored) and drive the CLI search
    inject = [MARK + " auto-run test binary",
              "Wait 3",
              "SYS:AFtest/RUNME #? SYS:Prefs >SYS:AFtest/out.txt",
              MARK + " end"]
    out, done = [], False
    for ln in ss:
        if (not done) and ln.strip().lower().startswith("c:loadwb"):
            out.append(ln); out.extend(inject); done = True
        else:
            out.append(ln)
    if not done:                                   # fallback: append
        out.extend(inject)
    write(SS, "\n".join(out))
    print("setup done; BBS autostart disabled, RUNME auto-run injected")

def setupmu():
    """Like setup(), but boots Sashimi + MuForce before RUNME so MuForce's
    hits are captured to SYS:AFtest/mu.log."""
    for p in (SS, US):
        if not os.path.exists(p + ".afbak"):
            shutil.copyfile(p, p + ".afbak")
    os.makedirs(AFT, exist_ok=True)

    us = read(US).split("\n")
    out = []
    for ln in us:
        s = ln.strip().lower()
        if s.startswith("run") and ("cnet:control" in s or "hsserver" in s):
            out.append(MARK + "-off " + ln)
        else:
            out.append(ln)
    write(US, "\n".join(out))

    ss = read(SS).split("\n")
    ss = [l for l in ss if MARK not in l]
    inject = [MARK + " mu",
              "Wait 3",
              "Run >SYS:AFtest/mu.log SYS:Sashimi/sashimi NOPROMPT BUFK=64",
              "Wait 2",
              "SYS:MMULib/MuTools/MuForce",
              "Wait 2",
              "SYS:AFtest/RUNME #? SYS:Prefs >SYS:AFtest/out.txt",
              MARK + " end"]
    out, done = [], False
    for ln in ss:
        if (not done) and ln.strip().lower().startswith("c:loadwb"):
            out.append(ln); out.extend(inject); done = True
        else:
            out.append(ln)
    if not done:
        out.extend(inject)
    write(SS, "\n".join(out))
    print("mu-setup done: Sashimi + MuForce + RUNME injected")

def use(name):
    src = os.path.join(OUT, name)
    if not os.path.exists(src):
        sys.exit("no such binary: " + src)
    os.makedirs(AFT, exist_ok=True)
    shutil.copyfile(src, os.path.join(AFT, "RUNME"))
    outp = os.path.join(AFT, "out.txt")
    if os.path.exists(outp): os.remove(outp)
    print("RUNME <-", name)

def restore():
    for p in (SS, US):
        if os.path.exists(p + ".afbak"):
            shutil.copyfile(p + ".afbak", p)
            os.remove(p + ".afbak")
    if os.path.isdir(AFT):
        shutil.rmtree(AFT)
    print("restored original boot files; AFtest removed")

# --- clean OS3.2 box (DH0: = /mnt/c/Amiga/Clean 030 323): launch the GUI for a
#     visual test, mirroring AmiGrep's setupgui. Reversible (.affbak backup). ---
CLEAN     = "/mnt/c/Amiga/Clean 030 323"
CLEAN_SS  = os.path.join(CLEAN, "S", "Startup-Sequence")
CLEAN_DST = os.path.join(CLEAN, "AmiFind")
CLEAN_MARK = ";AFFGUI"

def deploy_clean():
    os.makedirs(CLEAN_DST, exist_ok=True)
    for name in ("AmiFind", "AmiFindGUI", "AmiFindGUI.info"):
        src = os.path.join(OUT, name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(CLEAN_DST, name))
            print("deployed", name)
    print("-> DH0:AmiFind/ (clean 030 box)")

def setupgui():
    """Deploy to the clean box and auto-run AmiFindGUI on next boot."""
    deploy_clean()
    if not os.path.exists(CLEAN_SS + ".affbak"):
        shutil.copyfile(CLEAN_SS, CLEAN_SS + ".affbak")
    lines = [l for l in read(CLEAN_SS).split("\n") if CLEAN_MARK not in l]
    block = [CLEAN_MARK + " begin", "Wait 2",
             "Run DH0:AmiFind/AmiFindGUI", CLEAN_MARK + " end"]
    out, done = [], False
    for ln in lines:
        out.append(ln)
        if (not done) and ln.strip().lower().startswith("loadwb"):
            out.extend(block); done = True
    if not done:
        out.extend(block)
    write(CLEAN_SS, "\n".join(out))
    print("setupgui: AmiFindGUI will open on next boot of the clean 030 box")

def restoregui():
    if os.path.exists(CLEAN_SS + ".affbak"):
        shutil.copyfile(CLEAN_SS + ".affbak", CLEAN_SS)
        os.remove(CLEAN_SS + ".affbak")
        print("restored clean-box Startup-Sequence")
    else:
        print("(no clean-box backup to restore)")

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "setup":         setup()
    elif cmd == "setupmu":     setupmu()
    elif cmd == "use":         use(sys.argv[2])
    elif cmd == "restore":     restore()
    elif cmd == "setupgui":    setupgui()
    elif cmd == "restoregui":  restoregui()
    else: sys.exit(__doc__)
