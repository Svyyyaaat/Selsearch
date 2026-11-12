#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Selsearch lens smoothness test for Hyprland.

Restarts selsearchd under a few renderer/GPU setups, opens the lens, moves
the pointer across every monitor with `hyprctl dispatch movecursor`, and
prints what the SELSEARCH_PERF probe measured (frames per second, render
time, longest gap between frames, longest UI-thread stall, and which GPU
actually rendered). Takes about a minute; don't touch the mouse meanwhile.

    python3 perf-test.py            # all setups
    python3 perf-test.py default    # only some: default basic vulkan mesa nvidia
"""
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

VENDOR_DIR = "/usr/share/glvnd/egl_vendor.d"
SETUPS = [
    ("default", "as installed", {}),
    ("basic", "Qt single-threaded render loop", {"QSG_RENDER_LOOP": "basic"}),
    ("vulkan", "Vulkan instead of OpenGL", {"QSG_RHI_BACKEND": "vulkan"}),
    ("mesa", "render on the Mesa GPU (AMD/Intel)", {"__EGL_VENDOR_LIBRARY_FILENAMES": f"{VENDOR_DIR}/50_mesa.json"}),
    ("nvidia", "render on the NVIDIA GPU", {"__EGL_VENDOR_LIBRARY_FILENAMES": f"{VENDOR_DIR}/10_nvidia.json",
                                            "__NV_PRIME_RENDER_OFFLOAD": "1"}),
]
LINE = re.compile(r"selsearch\.perf: (\S+): fps=(\d+) render avg=([\d.]+)ms max=([\d.]+)ms \| "
                  r"longest frame gap=([\d.]+)ms \| pointer moves=(\d+) \| ui stall max=(\d+)ms")
INFO = re.compile(r"selsearch\.perf: (\S+): api=(\S+) gpu=\"([^\"]*)\".*?size=(\S+) dpr=(\S+) refresh=(\d+)")


def run(*cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def monitors():
    out = run("hyprctl", "monitors", "-j")
    if out.returncode != 0:
        sys.exit("hyprctl is not available; this test is for Hyprland.")
    mons = []
    for m in json.loads(out.stdout):
        scale = m.get("scale", 1) or 1
        mons.append({"name": m["name"], "x": m["x"], "y": m["y"],
                     "w": m["width"] / scale, "h": m["height"] / scale,
                     "desc": f'{m["name"]} {m["width"]}x{m["height"]}@{m.get("refreshRate", 0):.0f}Hz scale {scale}'})
    return mons


def move(x, y):
    return run("hyprctl", "dispatch", "movecursor", str(int(x)), str(int(y))).returncode == 0


def daemon_up(timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if run("selsearch", "status").returncode == 0:
            return True
        time.sleep(0.25)
    return False


def stop_daemon():
    run("selsearch", "quit")
    time.sleep(1.0)
    run("pkill", "-x", "selsearchd")
    time.sleep(0.5)


def test(setup_env, mons, logdir, name, manual):
    stop_daemon()
    log_path = os.path.join(logdir, f"{name}.log")
    env = dict(os.environ, SELSEARCH_PERF="1", SELSEARCH_NO_WELCOME="1", **setup_env)
    with open(log_path, "w") as log:
        proc = subprocess.Popen(["selsearchd"], env=env, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
    if not daemon_up():
        proc.kill()
        return None, log_path
    first = mons[0]
    move(first["x"] + first["w"] / 2, first["y"] + first["h"] / 2)
    run("selsearch", "lens")
    time.sleep(4.0)  # capture, text recognition, intro
    mark = os.path.getsize(log_path)
    if manual:
        print("   Move the mouse around over EVERY screen for 8 seconds... go!", flush=True)
        time.sleep(8.0)
        print("   stop.", flush=True)
    for m in ([] if manual else mons):  # circles over each monitor, ~4 s each
        cx, cy = m["x"] + m["w"] / 2, m["y"] + m["h"] / 2
        rx, ry = m["w"] * 0.35, m["h"] * 0.35
        t_end = time.time() + 4.0
        i = 0
        while time.time() < t_end:
            a = i * 2 * math.pi / 120
            move(cx + rx * math.cos(a), cy + ry * math.sin(a))
            i += 1
            time.sleep(0.004)
    time.sleep(1.2)
    run("selsearch", "lens")  # closes it
    time.sleep(0.5)
    stop_daemon()
    with open(log_path, errors="replace") as f:
        text = f.read()
    info = {m.group(1): m.groups()[1:] for m in INFO.finditer(text)}
    stats = {}
    for m in LINE.finditer(text[mark:]):
        screen, fps, ravg, rmax, gap, moves, stall = m.groups()
        if int(moves) == 0 and int(fps) == 0:
            continue
        s = stats.setdefault(screen, {"fps": [], "render": [], "gap": 0.0, "stall": 0, "moves": 0})
        if int(moves) > 0:
            s["fps"].append(int(fps))
            s["render"].append(float(ravg))
            s["moves"] += int(moves)
        s["gap"] = max(s["gap"], float(gap))
        s["stall"] = max(s["stall"], int(stall))
    return (info, stats), log_path


def main():
    if not shutil.which("selsearchd") or not shutil.which("selsearch"):
        sys.exit("selsearch is not installed (selsearchd not in PATH).")
    wanted = sys.argv[1:] or [s[0] for s in SETUPS]
    mons = monitors()
    manual = not move(mons[0]["x"] + 10, mons[0]["y"] + 10)
    if manual:
        print("`hyprctl dispatch movecursor` doesn't work here, so you'll move the mouse yourself when asked.")
    service_active = run("systemctl", "--user", "is-active", "--quiet", "selsearch.service").returncode == 0
    if service_active:
        run("systemctl", "--user", "stop", "selsearch.service")
    logdir = tempfile.mkdtemp(prefix="selsearch-perf-")

    gpus = [l for l in run("lspci").stdout.splitlines() if re.search(r"VGA|3D|Display", l)]
    print("GPUs:", *gpus, sep="\n  ")
    print("Monitors:", *[m["desc"] for m in mons], sep="\n  ")
    for var in ("AQ_DRM_DEVICES", "WLR_DRM_DEVICES", "__GLX_VENDOR_LIBRARY_NAME", "QSG_RENDER_LOOP", "QSG_RHI_BACKEND"):
        if os.environ.get(var):
            print(f"  {var}={os.environ[var]}")
    print("\nEach setup takes ~15 s." + ("" if manual else " Don't touch the mouse.") + "\n")

    rows = []
    for name, desc, env in SETUPS:
        if name not in wanted:
            continue
        if "__EGL_VENDOR_LIBRARY_FILENAMES" in env and not os.path.exists(env["__EGL_VENDOR_LIBRARY_FILENAMES"]):
            rows.append(f"{name:8} skipped ({os.path.basename(env['__EGL_VENDOR_LIBRARY_FILENAMES'])} not installed)")
            continue
        print(f"-> {name}: {desc}", flush=True)
        result, log = test(env, mons, logdir, name, manual)
        if result is None:
            rows.append(f"{name:8} FAILED to start (see {log})")
            continue
        info, stats = result
        for screen in sorted(set(info) | set(stats)):
            api, gpu, size, dpr, refresh = info.get(screen, ("?", "?", "?", "?", "?"))
            s = stats.get(screen)
            if s and s["fps"]:
                fps = sum(s["fps"]) / len(s["fps"])
                ren = sum(s["render"]) / len(s["render"])
                rows.append(f"{name:8} {screen:10} hover fps {fps:5.1f}  render {ren:5.2f} ms  "
                            f"worst gap {s['gap']:6.1f} ms  ui stall {s['stall']:4d} ms  | {api} {gpu[:48]} {size}@{refresh}Hz x{dpr}")
            else:
                rows.append(f"{name:8} {screen:10} no hover data  | {api} {gpu[:48]} {size}@{refresh}Hz x{dpr}")

    print("\n" + "\n".join(rows))
    print(f"\nLogs: {logdir}")
    if service_active:
        run("systemctl", "--user", "start", "selsearch.service")
        print("selsearch.service restarted.")
    else:
        print("Start selsearchd again the way you normally do (or `selsearch status` starts it).")


if __name__ == "__main__":
    main()
