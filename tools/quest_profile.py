#!/usr/bin/env python3
"""quest_profile.py -- WHERE THE QUEST'S FRAME GOES, live from the headset over adb.

No rebuild and no host: it reads what the headset already measures about whatever app is
running (a game's VR build, VR_PKG, or any other).
  - VrApi's once-a-second line (logcat): frame rate, the app's GPU time per frame (App=),
    CPU+GPU time, GPU/CPU busy, clock levels, temperature.
  - ovrgpuprofiler (Meta's GPU counter tool, on the headset): where the GPU's time goes --
    fragments vs vertices, fragments per frame (overdraw), ALU and texture fetches per
    fragment, bandwidth.
  - ovrgpuprofiler's render-stage trace: GPU time per render pass (the eye buffers, the
    HUD's 640x480 passes) and within a pass (render, MSAA resolve/store, depth store,
    preemption by the compositor).

  quest_profile.py [live] [seconds]     one row a second: VrApi + GPU counters (default 30 s; 0 = until Ctrl-C)
  quest_profile.py passes [seconds]     GPU time per render pass and stage (default trace 0.2 s)
  quest_profile.py passes --restart     ... restarting the app first (the trace needs the
                                        driver's detailed mode, which only applies to apps
                                        started after it is switched on)
  quest_profile.py counters [ids]       raw ovrgpuprofiler counters (ids: `adb shell ovrgpuprofiler -m`)
  quest_profile.py off                  switch the driver's detailed mode off again, and --awake's override
  --awake (any mode)                    keep the headset drawing with nobody wearing it (Meta's
                                        proximity-sensor override; `off` undoes it)

  -h, --help                            this text

  VR_PKG          the app's package (default com.doktorplopz.LetsGoAway; --restart starts it;
                  live filters VrApi by its pid)
  VR_ACTIVITY     its activity (default com.doktorplopz.letsgoaway.host.LetsGoAwayActivity)
  VR_PROFILE_DIR  where each run's raw output is kept (default ./profiles)
  ANDROID_SERIAL  the headset; needed when more than one is plugged in (adb reads it itself: no -s)
  ADB             the adb to use (default: adb on the PATH)
  VR_DRY_RUN=1    print the adb commands instead of running them (no headset needed)
  VR_DRY_DEVICES  a dry run only: the serials to pretend are plugged in, space separated

The budget is the headset's refresh: 13.9 ms at 72 Hz. Measured 2026-09-26 on a Quest 3,
Rave Racer's VR world build: 40/72 fps, App (GPU) 20-26 ms, GPU 99% busy, CPU 14%; 99.7% of
GPU time shading fragments.
"""
import os, re, subprocess, sys, threading, time, queue, datetime

ADB = os.environ.get("ADB", "adb")
PKG = os.environ.get("VR_PKG", "com.doktorplopz.LetsGoAway")
ACT = os.environ.get("VR_ACTIVITY", "com.doktorplopz.letsgoaway.host.LetsGoAwayActivity")
DRY = bool(os.environ.get("VR_DRY_RUN"))
OUT = os.path.abspath(os.environ.get("VR_PROFILE_DIR", "profiles"))
# the recommended eye buffer (scale 1.0) per headset, for overdraw = fragments / eye pixels
EYE = {"eureka": (1680, 1760), "hollywood": (1440, 1584), "seacliff": (1800, 1920), "panther": (1440, 1584)}

# ovrgpuprofiler's realtime counters (ids from `ovrgpuprofiler -m` on a Quest 3)
COUNTERS = {4: "GPU % Utilization", 32: "% Time Shading Fragments", 25: "% Time Shading Vertices",
            33: "Fragments Shaded / Second", 41: "ALU / Fragment", 39: "Textures / Fragment",
            8: "% Texture Fetch Stall", 22: "% Texture Pipes Busy", 70: "Average Polygon Area",
            64: "Pre-clipped Polygons/Second", 12: "Read Total (Bytes/sec)", 13: "Write Total (Bytes/sec)"}


def adb(*args, **kw):
    if DRY:
        print("+ adb", *args, file=sys.stderr)
        return ""
    return subprocess.run([ADB, *args], capture_output=True, text=True, **kw).stdout


def pick_device():
    """THE ONE HEADSET: ANDROID_SERIAL (adb reads it itself), or the only one plugged in; refused when
    more than one is plugged in and none is named, or when the one named is not attached. Leaves
    ANDROID_SERIAL set, so a headset plugged in later does not confuse a long run. Same rules as
    quest_device.sh."""
    if DRY and "VR_DRY_DEVICES" in os.environ:
        devs = [(d, "device") for d in os.environ["VR_DRY_DEVICES"].split()]
    else:
        out = subprocess.run([ADB, "devices"], capture_output=True, text=True).stdout.splitlines()[1:]
        devs = [tuple(l.split()[:2]) for l in out if len(l.split()) >= 2]
    if not devs and DRY:
        return
    ok = [d for d, st in devs if st == "device"]
    listed = "".join(f" {d} ({st})" for d, st in devs)
    want = os.environ.get("ANDROID_SERIAL", "")
    if want:
        state = dict(devs).get(want)
        if state is None:
            sys.exit(f"ANDROID_SERIAL={want} is not attached; attached:{listed or ' none'}")
        if state != "device":
            sys.exit(f"ANDROID_SERIAL={want} is {state}: allow USB debugging in the headset")
    elif len(ok) > 1:
        sys.exit(f"{len(ok)} headsets are plugged in and ANDROID_SERIAL is not set; attached:{listed}\n"
                 f"name one: ANDROID_SERIAL=<serial> {sys.argv[0]}")
    elif not ok:
        sys.exit("no device: plug the Quest in, allow USB debugging" + (f" (adb sees:{listed})" if listed else ""))
    else:
        os.environ["ANDROID_SERIAL"] = ok[0]
    print(f"# headset: {os.environ['ANDROID_SERIAL']}", file=sys.stderr)


def log_file(kind):
    os.makedirs(OUT, exist_ok=True)
    return open(os.path.join(OUT, f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{kind}.log"), "w")


def check_device():
    pick_device()
    dev = adb("shell", "getprop", "ro.product.device").strip()
    top = adb("shell", "dumpsys", "activity", "activities")
    m = re.search(r"ResumedActivity[=:] ?ActivityRecord\{\S+ \S+ ([\w.]+)/", top)
    if "--awake" in sys.argv:                        # nobody wearing it: keep it drawing
        adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close")
        adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
        if not DRY: time.sleep(3)
    if not DRY and "Wakefulness=Awake" not in adb("shell", "dumpsys", "power"):
        print("# the headset is asleep (nobody wearing it?): the app draws nothing. --awake keeps it on "
              "(the proximity override; `off` undoes it)")
    return dev, (m.group(1) if m else "?")


def vrapi_fields(line):
    """VrApi's line: 'FPS=40/72,Prd=65ms,...,App=25.62ms,...' -> dict"""
    body = line.split("VrApi", 1)[-1].split(":", 1)[-1]
    return dict(kv.split("=", 1) for kv in body.strip().split(",") if "=" in kv)


def num(s, default=float("nan")):
    m = re.match(r"-?[\d.]+", s or "")
    return float(m.group()) if m else default


def live(seconds):
    dev, top = check_device()
    if DRY:
        ids = ",".join(str(k) for k in COUNTERS)
        adb("logcat", "-c"); adb("shell", "pidof", PKG)
        adb("logcat", "-s", "VrApi:I", "--pid=<the pid>"); adb("shell", f'ovrgpuprofiler --realtime="{ids}"')
        adb("shell", "pkill", "ovrgpuprofiler")
        return
    print(f"# {dev}, in front: {top}")
    raw = log_file("live")
    q = queue.Queue()
    adb("logcat", "-c")
    # VrApi's line comes from every VR process (the system overlay's too, at its own eye
    # scale): only the app's own
    pid = adb("shell", "pidof", PKG).strip().split() if PKG else []
    lc = subprocess.Popen([ADB, "logcat", "-s", "VrApi:I"] + ([f"--pid={pid[0]}"] if pid else []), stdout=subprocess.PIPE, text=True)
    print(f"# VrApi of {PKG} (pid {pid[0]})" if pid else f"# {PKG or 'no VR_PKG'} not running: VrApi of every process")
    ids = ",".join(str(k) for k in COUNTERS)
    gp = subprocess.Popen([ADB, "shell", f'ovrgpuprofiler --realtime="{ids}"'], stdout=subprocess.PIPE, text=True)

    def pump(tag, p):
        for line in p.stdout: q.put((tag, line.rstrip("\n")))
    for tag, p in (("vrapi", lc), ("gpu", gp)):
        threading.Thread(target=pump, args=(tag, p), daemon=True).start()

    ctr = {}                                            # the newest value of each counter
    eye = None
    head = (f"{'fps':>7} {'GPU ms':>6} {'C&G ms':>6} {'GPU%':>4} {'CPU%':>4} {'lvl c/g':>7} {'MHz c/g':>9} {'temp':>5} {'foveat':>6} {'eye x':>5} | "
            f"{'frag%':>5} {'Mfrag/f':>7} {'overdraw':>8} {'ALU/fr':>6} {'tex/fr':>6} {'texstall':>8} {'poly px':>7} {'GB/s r+w':>8}")
    print(head); raw.write(head + "\n")
    t_end = time.time() + seconds if seconds > 0 else float("inf")
    rows = []
    try:
        while time.time() < t_end:
            try: tag, line = q.get(timeout=1)
            except queue.Empty: continue
            raw.write(f"{tag}\t{line}\n")
            if tag == "gpu":
                m = re.match(r"\s*(.+?)\s+:\s+([-\d.]+)", line)
                if m: ctr[m.group(1)] = float(m.group(2))
                continue
            if "FPS=" not in line: continue
            f = vrapi_fields(line)
            fps = num(f.get("FPS"))
            sf = num(f.get("SF"), 1.0)
            if dev in EYE: eye = (round(EYE[dev][0] * sf), round(EYE[dev][1] * sf))
            fr_s = ctr.get(COUNTERS[33], float("nan"))
            mfrag = fr_s / fps / 1e6 if fps > 0 else float("nan")
            over = mfrag * 1e6 / (2 * eye[0] * eye[1]) if eye else float("nan")
            lv = re.match(r"(\d+)/(\d+)", f.get("CPU4/GPU", "") or "")
            bw = (ctr.get(COUNTERS[12], 0) + ctr.get(COUNTERS[13], 0)) / 1e9
            row = (f"{f.get('FPS', '?'):>7} {num(f.get('App')):6.2f} {num(f.get('CPU&GPU')):6.2f} {num(f.get('GPU%')) * 100:4.0f} "
                   f"{num(f.get('CPU%')) * 100:4.0f} {(lv.group(1) + '/' + lv.group(2)) if lv else '?':>7} "
                   f"{f.get('CPU4/GPU', '').split(',')[-1] if ',' in f.get('CPU4/GPU', '') else '':>9} {f.get('Temp', '?').split('/')[0]:>5} "
                   f"{f.get('Fov', '?'):>6} {sf:5.2f} | "
                   f"{ctr.get(COUNTERS[32], float('nan')):5.1f} {mfrag:7.1f} {over:8.1f} {ctr.get(COUNTERS[41], float('nan')):6.0f} "
                   f"{ctr.get(COUNTERS[39], float('nan')):6.2f} {ctr.get(COUNTERS[8], float('nan')):8.1f} "
                   f"{ctr.get(COUNTERS[70], float('nan')):7.0f} {bw:8.2f}")
            print(row, flush=True); raw.write(row + "\n")
            rows.append((num(f.get("App")), fps, mfrag))
    except KeyboardInterrupt:
        pass
    finally:
        lc.terminate(); gp.terminate()
        adb("shell", "pkill", "ovrgpuprofiler")         # the device-side process outlives adb
    if rows:
        ok = [r for r in rows if r[0] == r[0]]
        if ok:
            app = sorted(r[0] for r in ok)
            s = (f"# {len(ok)} s: GPU ms mean {sum(app) / len(app):.2f} p50 {app[len(app) // 2]:.2f} max {app[-1]:.2f}; "
                 f"fps mean {sum(r[1] for r in ok) / len(ok):.1f}")
            print(s); raw.write(s + "\n")
    print(f"# raw: {raw.name}")
    raw.close()


SURF = re.compile(r"Surface\s+(\d+)\s*\|\s*(\d+)\s*x\s*(\d+)\s*\|\s*([^|]*?)\|\s*([^|]*?)\|\s*([\d.]+)\s*ms\s*\|\s*\d+\s+stages\s*:(.*)")


def passes(seconds, restart):
    dev, top = check_device()
    if DRY:
        adb("shell", "ovrgpuprofiler", "-i"); adb("shell", "ovrgpuprofiler", "-e")
        if restart:
            adb("shell", "am", "force-stop", PKG); adb("shell", "am", "start", "-n", f"{PKG}/{ACT}")
        adb("shell", f"ovrgpuprofiler -t {seconds}")
        return
    on = "enabled" in adb("shell", "ovrgpuprofiler", "-i")
    if not on:
        print(adb("shell", "ovrgpuprofiler", "-e").strip().splitlines()[-1])
    if restart or not on:
        if not PKG:
            sys.exit("set VR_PKG to the app's package")
        if not restart:
            sys.exit("detailed mode is on now, but only for apps started after it: run again with --restart "
                     f"(restarts {PKG}), or restart the app by hand")
        adb("shell", "am", "force-stop", PKG)
        adb("shell", "am", "start", "-n", f"{PKG}/{ACT}")
        print(f"# restarted {PKG}; waiting 20 s for it to settle"); time.sleep(20)
    out = adb("shell", f"ovrgpuprofiler -t {seconds}")
    raw = log_file("passes"); raw.write(out); raw.close()
    kinds = {}                                           # (w, h, format) -> [ms, {stage: ms}, n]
    for line in out.splitlines():
        m = SURF.search(line)
        if not m: continue
        w, h, fmt, bins, ms, stages = int(m[2]), int(m[3]), m[4].strip(), m[5].strip(), float(m[6]), m[7]
        k = kinds.setdefault((w, h, fmt, bins), [0.0, {}, 0])
        k[0] += ms; k[2] += 1
        for name, v in re.findall(r"(\w+)\s*:\s*([\d.]+)ms", stages):
            k[1][name] = k[1].get(name, 0) + float(v)
    if not kinds:
        print(out); sys.exit("no surfaces in the trace (is the app drawing? was it started after detailed mode?)")
    # a frame = one execution of the biggest surface (the eye buffers: both eyes in one pass, multiview)
    big = max(kinds, key=lambda k: k[0] * k[1])
    frames = kinds[big][2]
    total = sum(v[0] for v in kinds.values()) / frames
    print(f"# {dev}, {top}: {frames} frames traced; GPU per frame {total:.2f} ms over all passes")
    for (w, h, fmt, bins), (ms, st, n) in sorted(kinds.items(), key=lambda kv: -kv[1][0]):
        per = ms / frames
        print(f"{w}x{h} x{n / frames:.0f}/frame  {per:6.2f} ms/frame ({100 * per / total:4.1f}%)  [{fmt}; {bins}]")
        listed = sum(st.values()) / frames
        for name, v in sorted(st.items(), key=lambda kv: -kv[1]):
            print(f"      {name:<18} {v / frames:6.2f} ms")
        if per - listed > 0.05:
            print(f"      {'(not itemised)':<18} {per - listed:6.2f} ms")
    print(f"# raw: {raw.name}")


def main():
    if "-h" in sys.argv or "--help" in sys.argv:
        print(__doc__); return
    a = [x for x in sys.argv[1:] if x != "--awake"]
    mode = a.pop(0) if a and not re.match(r"[\d.]+$", a[0]) else "live"
    if mode == "live":
        live(float(a[0]) if a else 30)
    elif mode == "passes":
        restart = "--restart" in a
        a = [x for x in a if x != "--restart"]
        passes(float(a[0]) if a else 0.2, restart)
    elif mode == "counters":
        check_device()
        ids = a[0] if a else ",".join(str(k) for k in COUNTERS)
        try:
            if DRY: adb("shell", f'ovrgpuprofiler --realtime="{ids}"')
            else: subprocess.run([ADB, "shell", f'ovrgpuprofiler --realtime="{ids}"'])
        except KeyboardInterrupt: pass
        finally: adb("shell", "pkill", "ovrgpuprofiler")
    elif mode == "off":
        print(adb("shell", "ovrgpuprofiler", "-d").strip())
        adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.automation_disable")
        print("proximity sensor back to normal")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
