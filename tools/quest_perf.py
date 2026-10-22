#!/usr/bin/env python3
"""Unattended performance runs on a Quest over adb.

  quest_perf.py run [--stage N] [--players 2|4] [--phases mr:40,vr:40] [--env K=V ...]
                    [--warmup S] [--label NAME] [--cpu-level N] [--gpu-level N]
                    [--record NAME | --replay NAME]
      Boots a four-CPU match on stage N (StKind, gr/forward.h; 31 is
      Battlefield), then holds each phase (mode:seconds) in turn in the same
      session and prints the runtime's frame stats for each one. The arena is
      placed in front of wherever the headset lies (AURORA_XR_ARENA_AT_HEAD),
      and one eye image dumped in the warmup checks the stage is in view. Logs go to
      build/quest-perf/<label>-<phase>.log.
  quest_perf.py mode mr|vr      switch the running game (MELEE_XR_CONTROL)
  quest_perf.py summarize LOG   summarize a saved logcat
  quest_perf.py grant           grant the runtime permissions the launcher
                                panel would ask for (every run does this too)

Benchmarks: --cpu-level / --gpu-level pin the clocks for the run
(debug.oculus.cpuLevel / gpuLevel, cleared after), e.g. 3 and 2, the caps
Meta documents for apps showing passthrough, so numbers don't ride on clock
levels the runtime may not grant. --record NAME saves the match's seed and
pads (MELEE_NET_RECORD) to the app's files dir; --replay NAME plays them back
(MELEE_NET_REPLAY), so runs simulate the same frames and are comparable; the
summary says whether the replay stayed in step.

The game reads its knobs from melee-env.txt (src/pc/main.c,
pc_env_file_bootstrap), which this rewrites on every run; the player's own
is kept aside (melee-env.txt.player) and put back after each run and at
hand-back. The disc comes from
the launcher's own launcher.cfg (debug builds only: run-as), else the one the
game last started with (files/last-disc.txt, written by MeleeActivity), or
MELEE_DISC.
With more than one adb device and no ANDROID_SERIAL, the first device listed
by address is used: a wireless headset shows up under both its address and
its mDNS name.

Headset power: a run wakes the headset (reconnecting wireless adb if it
dropped while dozing), reports proximity closed and sets a 30-minute screen
timeout, so it stays on unworn through a batch of runs; each run ends by
stopping the game and deleting its run files. At the end of a batch,
`quest_perf.py handback` (or --hand-back on the last run) hands proximity back
to the sensor and puts Android's screen timeout back out of the way
(999999999 ms: the headset's own power manager sleeps it once it's off a
head; a short Android timeout darkens a worn one, as controllers in VR
don't count as activity). Once the headset sleeps
deeply its wifi drops and only a person can wake it, so don't hand back
between runs. `quest_perf.py wake` keeps it on without a run.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time

# The application id: Xelee Watch's by default; --pkg (or MELEE_ANDROID_PKG)
# for another build, such as the official app's dev.melee.game.
PKG = os.environ.get("MELEE_ANDROID_PKG", "dev.melee.game.watch")
FILES = f"/sdcard/Android/data/{PKG}/files"
ENV_FILE = f"{FILES}/melee-env.txt"
CTL_FILE = f"{FILES}/melee-ctl.txt"
# The game's own log (MELEE_LOG_FILE) for record/replay runs: logcat on a
# Quest turns over within seconds, taking the replay's verdict with it.
GAME_LOG = f"{FILES}/melee-log.txt"
LOG_BUFFER = "64M"  # logcat ring size for a run (adb logcat -G)
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "build", "quest-perf")


def adb(*args, check=True, capture=True):
    r = subprocess.run(["adb", *args], text=True, capture_output=capture)
    if check and r.returncode != 0:
        sys.exit(f"adb {' '.join(args)} failed: {r.stderr.strip()}")
    return r.stdout if capture else ""


def pick_device():
    if os.environ.get("ANDROID_SERIAL"):
        return
    devs = [l.split()[0] for l in adb("devices").splitlines()[1:] if l.endswith("\tdevice")]
    if not devs:
        sys.exit("no adb device")
    by_addr = [d for d in devs if re.match(r"^\d+\.\d+\.\d+\.\d+:\d+$", d)]
    os.environ["ANDROID_SERIAL"] = (by_addr or devs)[0]


RUN_SCREEN_TIMEOUT_MS = 14400000  # during a batch: the display stays on unworn, 4 h (gaps between runs too)
# After it: Android's own timeout stays out of the way, as the headset has it.
# The Quest sleeps an unworn headset itself (its power manager's auto-sleep
# timer, after the proximity sensor opens); Android's timeout counts only
# Android input (a gamepad, the volume keys), not the controllers in VR, so
# 60 s of it darkened a worn headset in the middle of a fight.
IDLE_SCREEN_TIMEOUT_MS = 999999999


def wake_headset(tries=36):
    """Reconnects wifi adb if it dropped while the headset slept, then wakes
    it (proximity reported closed, a wake key) until it says it's awake. A
    dozing headset's wifi comes and goes, so a command that hangs is just a
    failed try."""
    serial = os.environ.get("ANDROID_SERIAL", "")

    def quiet(*args):
        try:
            return subprocess.run(["adb", *args], text=True, capture_output=True, timeout=20).stdout
        except subprocess.TimeoutExpired:
            return ""

    for _ in range(tries):
        if quiet("get-state").strip() != "device":
            if ":" in serial:
                quiet("disconnect", serial)
                quiet("connect", serial)
            time.sleep(5)
            continue
        if "Awake" in quiet("shell", "dumpsys power | grep mWakefulness="):
            return
        quiet("shell", "am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null; input keyevent KEYCODE_WAKEUP")
        time.sleep(5)
    sys.exit("the headset didn't wake")


# A run's melee-env.txt starts with this line; any other is the player's
# own, kept aside as ENV_SAVED while runs replace it and put back after.
RUN_ENV_MARK = "# quest_perf run\n"
ENV_SAVED = ENV_FILE + ".player"


def save_player_env():
    """Before a run writes its env: the player's own melee-env.txt (one
    without the run mark), if any and not saved already, aside."""
    current = adb("shell", "cat", ENV_FILE, check=False)
    if not current or current.startswith(RUN_ENV_MARK.strip()) or "No such file" in current:
        return
    if adb("shell", "ls", ENV_SAVED, check=False).strip() == ENV_SAVED:
        return
    adb("shell", "cp", ENV_FILE, ENV_SAVED, check=False)


def remove_run_files():
    """The run's env and control files gone, the player's env back."""
    adb("shell", "rm", "-f", CTL_FILE, check=False)
    if adb("shell", "ls", ENV_SAVED, check=False).strip() == ENV_SAVED:
        adb("shell", f"mv {ENV_SAVED} {ENV_FILE} && chmod 666 {ENV_FILE}", check=False)
    else:
        current = adb("shell", "cat", ENV_FILE, check=False)
        if current.startswith(RUN_ENV_MARK.strip()):
            adb("shell", "rm", "-f", ENV_FILE, check=False)


def hand_back():
    """The end of a batch: the game stopped, its run files gone (the
    player's own melee-env.txt back), proximity back to the sensor and the
    headset's own long screen timeout, so the unworn headset sleeps by its
    auto-sleep timer and the worn one doesn't."""
    adb("shell", "am", "force-stop", PKG, check=False)
    remove_run_files()
    adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.automation_disable", check=False)
    adb("shell", "settings", "put", "system", "screen_off_timeout", str(IDLE_SCREEN_TIMEOUT_MS), check=False)


# Runtime permissions the game asks for in the launcher panel, which an
# unattended run never shows: granted up front, so no dialog waits on anyone
# and the features behind them run as they would for a player who allowed
# them. USE_SCENE (both names): the stage's shadow on the surface under it;
# HEADSET_CAMERA: room capture, for XR room lighting.
PERMISSIONS = [
    "horizonos.permission.USE_SCENE",
    "com.oculus.permission.USE_SCENE",
    "horizonos.permission.HEADSET_CAMERA",
]


def grant_permissions():
    """pm grant each of PERMISSIONS; one the device doesn't know, or the
    installed build doesn't declare, is reported and skipped."""
    for perm in PERMISSIONS:
        r = subprocess.run(["adb", "shell", "pm", "grant", PKG, perm], text=True, capture_output=True)
        if r.returncode != 0:
            msg = (r.stderr or r.stdout).strip().splitlines()
            print(f"  pm grant {perm}: {msg[0] if msg else 'failed'}")


def shell_write(path, text):
    # chmod: the shell's umask leaves the file unreadable to the app.
    subprocess.run(["adb", "shell", f"cat > {path} && chmod 644 {path}"], input=text, text=True, check=True)


def disc_uri():
    if os.environ.get("MELEE_DISC"):
        return os.environ["MELEE_DISC"]
    cfg = adb("shell", "run-as", PKG, "cat", "files/launcher.cfg", check=False)
    m = re.search(r'^disc "(.*)"$', cfg, re.M)
    if m:
        return m.group(1)
    # A release build (no run-as): the disc the game last started with
    # (MeleeActivity.getArguments).
    last = adb("shell", "cat", f"{FILES}/last-disc.txt", check=False).strip()
    if last and "No such file" not in last:
        return last
    sys.exit("no disc in launcher.cfg or last-disc.txt (play once from the launcher); set MELEE_DISC")


# VrApi's once-a-second line, e.g.
# FPS=121/120,...,Stale=4,...,CPU4/GPU=6/2,2361/545MHz,...,App=5.71ms,...,GPU%=0.84,CPU%=0.55(W0.91)
VRAPI = re.compile(
    r"FPS=(\d+)/(\d+).*?Stale=(\d+).*?CPU4/GPU=(\d+)/(\d+),(\d+)/(\d+)MHz.*?"
    r"App=([\d.]+)ms.*?GPU%=([\d.]+),CPU%=([\d.]+)"
)
XR_FPS = re.compile(r"([\d.]+) display fps \((\d+)% 3D\); frames/s released: screen ([\d.]+), 3D ([\d.]+), HUD ([\d.]+)")
PIPELINES = re.compile(r"pipelines: (\d+) created .*?Dawn cache: (\d+) hits.*?(\d+) misses")
XR_GPU = re.compile(r"GPU 3D passes per frame \(us\): 3D eyes (\d+).*?\(total (\d+)\); (\d+) world draws")


PID = re.compile(r"^\S+ \S+ [VDIWE]/(\S+)\s*\(\s*(\d+)\)")


def game_lines(text):
    """The game's own lines: other apps (the Home shell) log VrApi stats too.
    The game is the process writing Aurora lines."""
    lines = text.splitlines()
    pids = {m.group(2) for m in map(PID.match, lines) if m and m.group(1) == "Aurora"}
    return [l for l in lines if (m := PID.match(l)) and m.group(2) in pids] if pids else lines


def summarize(text):
    lines = game_lines(text)
    rows = [m.groups() for m in map(VRAPI.search, lines) if m]
    if not rows:
        return "  no VrApi stats (is the game in front and the headset awake?)"

    def col(i, f=float):
        return [f(r[i]) for r in rows]

    def mean(v):
        return statistics.fmean(v)

    def levels(i):
        vals = col(i, int)
        return "/".join(f"{k}:{vals.count(k)}" for k in sorted(set(vals)))

    out = [
        f"  {len(rows)} s  fps {mean(col(0)):.1f}/{rows[0][1]}  stale/s {mean(col(2)):.1f} (max {max(col(2, int))})",
        f"  CPU level {levels(3)} @ {mean(col(5)):.0f} MHz   GPU level {levels(4)} @ {mean(col(6)):.0f} MHz",
        f"  GPU% {mean(col(8)):.2f}  CPU% {mean(col(9)):.2f}  App GPU {mean(col(7)):.2f} ms",
    ]
    fps = [m.groups() for m in map(XR_FPS.search, lines) if m]
    if fps:
        out.append(f"  game: 3D released {mean([float(f[3]) for f in fps]):.1f}/s, "
                   f"HUD {mean([float(f[4]) for f in fps]):.1f}/s ({len(fps)} samples)")
    # Pipelines still compiling (a new build or Dawn invalidates the cache)
    # load the CPU and make the run meaningless.
    pipes = [m.groups() for m in map(PIPELINES.search, lines) if m]
    if pipes and int(pipes[-1][2]) > 500:  # cumulative; a warm cache still misses ~100 at boot
        out.insert(0, f"  WARNING: pipeline cache cold ({pipes[-1][2]} misses so far); rerun")
    gpu = [m.groups() for m in map(XR_GPU.search, lines) if m]
    if gpu:
        out.append(f"  passes: 3D eyes {mean([int(g[0]) for g in gpu]):.0f} us, "
                   f"total {mean([int(g[1]) for g in gpu]):.0f} us, "
                   f"{mean([int(g[2]) for g in gpu]):.0f} world draws")
    return "\n".join(out)


DUMP_DIR = f"{FILES}/dump"


def set_package(pkg):
    """Point every on-device path at another application id's files (--pkg)."""
    global PKG, FILES, ENV_FILE, CTL_FILE, GAME_LOG, ENV_SAVED, DUMP_DIR
    PKG = pkg
    FILES = f"/sdcard/Android/data/{PKG}/files"
    ENV_FILE = f"{FILES}/melee-env.txt"
    CTL_FILE = f"{FILES}/melee-ctl.txt"
    GAME_LOG = f"{FILES}/melee-log.txt"
    ENV_SAVED = ENV_FILE + ".player"
    DUMP_DIR = f"{FILES}/dump"


def view_coverage(label):
    """How much of the left eye the 3D view drew (its alpha), from the dump,
    which is kept as build/quest-perf/<label>-eye.pgm to look at."""
    raw = subprocess.run(["adb", "exec-out", "cat", f"{DUMP_DIR}/xr_3d_alpha.pgm"], capture_output=True).stdout
    adb("shell", "rm", "-rf", DUMP_DIR, check=False)
    if raw:
        with open(os.path.join(OUT_DIR, f"{label}-eye.pgm"), "wb") as f:
            f.write(raw)
    parts = raw.split(maxsplit=4)
    if len(parts) < 5 or parts[0] != b"P5":
        return "  view: no eye dump (WARNING: can't tell whether the stage is in view)"
    w, h = int(parts[1]), int(parts[2])
    data = parts[4]
    # Multiview dumps stack the eyes vertically; the left eye is the top half.
    eye = data[: w * (h // 2)] if h > w else data
    covered = sum(1 for i in range(0, len(eye), 97) if eye[i] > 0) / max(len(range(0, len(eye), 97)), 1)
    warn = "  WARNING: stage barely in view; the GPU numbers are meaningless" if covered < 0.02 else ""
    return f"  view: the 3D view covers {covered * 100:.1f}% of the left eye{warn}"


def set_levels(cpu, gpu):
    """Pins (or, with None, releases) the CPU and GPU clock levels. -1 releases
    the override (the runtime logs "override released (value=-1)"); an empty
    value left it at level 0, so the GPU ran at 285 MHz in normal play after
    a benchmark (2026-10-15)."""
    for prop, lvl in (("debug.oculus.cpuLevel", cpu), ("debug.oculus.gpuLevel", gpu)):
        adb("shell", "setprop", prop, "-1" if lvl is None else str(lvl), check=False)


def replay_status(text):
    """What the game said about a replay: whether it diverged, and where."""
    lines = [l for l in text.splitlines() if "REPLAY DIVERGED" in l or "replay finished" in l or "net: replaying" in l
             or "net: recording" in l or "cannot open" in l]
    return "\n".join("  " + re.sub(r".*?net: ", "net: ", l) for l in lines[-3:])


def run(args):
    try:
        if args.cpu_level is not None or args.gpu_level is not None:
            set_levels(args.cpu_level, args.gpu_level)
        run_phases(args)
    finally:
        adb("shell", "am", "force-stop", PKG, check=False)
        remove_run_files()
        if args.cpu_level is not None or args.gpu_level is not None:
            set_levels(None, None)
        if args.hand_back:
            hand_back()


def run_phases(args):
    phases = []
    for p in args.phases.split(","):
        mode, secs = p.split(":")
        if mode not in ("mr", "vr"):
            sys.exit(f"phase mode must be mr or vr: {p}")
        phases.append((mode, int(secs)))
    env = {
        "MELEE_BOOT_SCENE": "vs",
        "MELEE_DEBUG_VS": f"cpu{args.players}",
        "MELEE_DEBUG_VS_STAGE": str(args.stage),
        "AURORA_XR_TIMING": "1",
        "MELEE_XR_MODE": phases[0][0],
        "MELEE_XR_CONTROL": CTL_FILE,
        # Nobody wears the headset: place the arena from wherever it lies
        # and looks, or the stage can be out of view and the GPU numbers
        # meaningless (seen 2026-10-09: an empty eye at 7 ms).
        "AURORA_XR_ARENA_AT_HEAD": "1",
    }
    if args.view_check:
        # One eye image during the warmup, after the fight starts and before
        # measuring, to check the stage is in view.
        env["AURORA_XR_DUMP"] = DUMP_DIR
        env["AURORA_XR_DUMP_AFTER"] = str(max(args.warmup - 15, 5) * 60)
    if args.record:
        env["MELEE_NET_RECORD"] = f"{FILES}/{args.record}"
    if args.replay:
        env["MELEE_NET_REPLAY"] = f"{FILES}/{args.replay}"
    if args.record or args.replay:
        env["MELEE_LOG_FILE"] = GAME_LOG
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v
    disc = disc_uri()
    if "AURORA_XR_DUMP" in env:
        adb("shell", "mkdir", "-p", env["AURORA_XR_DUMP"])
    wake_headset()
    save_player_env()
    shell_write(ENV_FILE, RUN_ENV_MARK + "".join(f"{k}={v}\n" for k, v in env.items()))
    shell_write(CTL_FILE, phases[0][0] + "\n")
    # Keep the display running with nobody wearing it, for the run.
    adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close")
    adb("shell", "settings", "put", "system", "screen_off_timeout", str(RUN_SCREEN_TIMEOUT_MS))
    adb("shell", "am", "force-stop", PKG)
    grant_permissions()
    # The headset logs so much that the default buffer turned over in ~20 s:
    # a phase's log held only its last two stats windows.
    adb("logcat", "-G", LOG_BUFFER, check=False)
    adb("logcat", "-c")
    adb("shell", "am", "start", "-n", f"{PKG}/dev.melee.MeleeXrActivity", "--es", "disc", disc)
    print(f"{args.label}: stage {args.stage}, {args.players} CPUs, {' '.join(args.env) or 'default env'}; "
          f"warming up {args.warmup} s")
    time.sleep(args.warmup)
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, f"{args.label}-warmup.log"), "w") as f:
        f.write(adb("logcat", "-d", "-v", "time"))
    if args.view_check:
        print(view_coverage(args.label))
    for i, (mode, secs) in enumerate(phases):
        if i > 0:
            shell_write(CTL_FILE, mode + "\n")
            time.sleep(args.settle)
        adb("logcat", "-c")
        time.sleep(secs)
        text = adb("logcat", "-d", "-v", "time")
        path = os.path.join(OUT_DIR, f"{args.label}-{i}-{mode}.log")
        with open(path, "w") as f:
            f.write(text)
        print(f"[{mode}] {path}\n{summarize(text)}")
        if args.record or args.replay:
            game_log = adb("shell", "cat", GAME_LOG, check=False)
            with open(os.path.join(OUT_DIR, f"{args.label}-game.log"), "w") as f:
                f.write(game_log)
            print(replay_status(game_log) or "  record/replay: no word from the game (not started?)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pkg", default=PKG, help=f"the app's application id (default {PKG})")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--stage", type=int, default=31)
    r.add_argument("--players", type=int, choices=[2, 4], default=4, help="CPU fighters")
    r.add_argument("--phases", default="mr:40,vr:40")
    r.add_argument("--env", action="append", default=[], help="extra K=V for melee-env.txt")
    r.add_argument("--warmup", type=int, default=50, help="seconds from launch to the first phase")
    r.add_argument("--settle", type=int, default=12, help="seconds after a mode switch before measuring")
    r.add_argument("--label", default="run")
    r.add_argument("--cpu-level", type=int, help="pin the CPU clock level for the run (e.g. 3)")
    r.add_argument("--gpu-level", type=int, help="pin the GPU clock level for the run (e.g. 2)")
    rr = r.add_mutually_exclusive_group()
    rr.add_argument("--record", help="record the match (seed and pads) to this file in the app's files dir")
    rr.add_argument("--replay", help="replay a match recorded with --record")
    r.add_argument("--no-view-check", dest="view_check", action="store_false",
                   help="skip dumping an eye image to check the stage is in view")
    r.add_argument("--hand-back", action="store_true",
                   help="afterwards, let the headset sleep (hand_back): for the last run of a batch")
    m = sub.add_parser("mode")
    m.add_argument("mode", choices=["mr", "vr"])
    s = sub.add_parser("summarize")
    s.add_argument("log")
    sub.add_parser("wake", help="wake the headset and keep it on unworn")
    sub.add_parser("handback", help="let the headset sleep: the end of a batch")
    sub.add_parser("grant", help="grant the game's runtime permissions (also done by every run)")
    args = ap.parse_args()
    set_package(args.pkg)
    if args.cmd == "summarize":
        with open(args.log) as f:
            print(summarize(f.read()))
        return
    pick_device()
    if args.cmd == "mode":
        shell_write(CTL_FILE, args.mode + "\n")
    elif args.cmd == "wake":
        wake_headset()
        adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close")
        adb("shell", "settings", "put", "system", "screen_off_timeout", str(RUN_SCREEN_TIMEOUT_MS))
    elif args.cmd == "handback":
        hand_back()
    elif args.cmd == "grant":
        grant_permissions()
    else:
        run(args)


if __name__ == "__main__":
    main()
