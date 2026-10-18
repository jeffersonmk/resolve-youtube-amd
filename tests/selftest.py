#!/usr/bin/env python3
"""End-to-end self test: render through the plugin with a headless Resolve Studio and verify the files.

1. Start Resolve headless:   /opt/resolve/bin/resolve -nogui &
2. Run:                      /usr/bin/python3 tests/selftest.py

Creates the project "yta_plugin_selftest" (a 10 s ProRes test clip with a beep), renders it with each
plugin preset plus a native ProRes reference, then checks codec, bit depth, AAC, faststart, A/V sync
and PSNR against the reference. Output goes to ~/Videos/yta_selftest (Resolve refuses hidden dirs).
"""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import install_presets as presets  # noqa: E402

sys.path.append("/opt/resolve/Developer/Scripting/Modules")
os.environ.setdefault("RESOLVE_SCRIPT_API", "/opt/resolve/Developer/Scripting")
os.environ.setdefault("RESOLVE_SCRIPT_LIB", "/opt/resolve/libs/Fusion/fusionscript.so")
import DaVinciResolveScript as dvr  # noqa: E402

OUT = Path.home() / "Videos" / "yta_selftest"
PROJECT = "yta_plugin_selftest"
FPS = 24
BEEP_AT = 2.0  # seconds; a 1-frame white flash and a beep happen together here


def sh(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def make_source(path):
    # Busy picture (moving gradients + noise, BT.709, inside the legal gamut) with a white flash at BEEP_AT,
    # and stereo audio with a beep at the same moment.
    flash = f"between(t,{BEEP_AT},{BEEP_AT + 1 / FPS - 0.001})"
    video = (f"testsrc2=s=1920x1080:r={FPS}:d=10,eq=saturation=0.6,noise=alls=6:allf=t,"
             f"drawbox=c=white:t=fill:enable='{flash}',"
             "scale=out_color_matrix=bt709:out_range=tv,format=yuv422p10le,"
             "setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv")
    sh("ffmpeg", "-v", "error", "-y",
       "-f", "lavfi", "-i", video,
       "-f", "lavfi", "-i", f"aevalsrc='0.5*sin(2*PI*1000*t)*between(t,{BEEP_AT},{BEEP_AT}+0.05)"
                            f"|0.5*sin(2*PI*1000*t)*between(t,{BEEP_AT},{BEEP_AT}+0.05)':s=48000:d=10",
       "-c:v", "prores_ks", "-profile:v", "3", "-c:a", "pcm_s16le", "-color_primaries", "bt709",
       "-color_trc", "bt709", "-colorspace", "bt709", str(path))


def connect():
    for _ in range(60):
        r = dvr.scriptapp("Resolve")
        if r:
            return r
        time.sleep(2)
    sys.exit("Resolve is not reachable - start it with: /opt/resolve/bin/resolve -nogui")


def setup_project(resolve, src):
    pm = resolve.GetProjectManager()
    if PROJECT in (pm.GetProjectListInCurrentFolder() or []):
        pm.LoadProject(PROJECT)
    else:
        pm.CreateProject(PROJECT)
    p = pm.GetCurrentProject()
    mp = p.GetMediaPool()
    # start clean every run so the timeline always uses the freshly generated source
    timelines = [p.GetTimelineByIndex(i + 1) for i in range(p.GetTimelineCount())]
    if timelines:
        mp.DeleteTimelines(timelines)
    clips = mp.GetRootFolder().GetClipList() or []
    if clips:
        mp.DeleteClips(clips)
    mp.CreateTimelineFromClips("selftest", mp.ImportMedia([str(src)]))
    p.SetCurrentTimeline(p.GetTimelineByIndex(1))
    return p


def render(p, label):
    p.SetRenderSettings({"TargetDir": str(OUT), "CustomName": label, "SelectAllFrames": True})
    p.DeleteAllRenderJobs()
    job = p.AddRenderJob()
    if not job:
        return None, "AddRenderJob failed"
    t0 = time.time()
    p.StartRendering(job)
    while p.IsRenderingInProgress():
        time.sleep(0.5)
    status = p.GetRenderJobStatus(job)
    return time.time() - t0, status.get("JobStatus")


def probe(path):
    return json.loads(sh("ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)))


def faststart(path):
    head = path.read_bytes()[: 1 << 20]
    moov, mdat = head.find(b"moov"), head.find(b"mdat")
    return moov != -1 and (mdat == -1 or moov < mdat)


def psnr(a, b):
    log = subprocess.run(["ffmpeg", "-hide_banner", "-i", str(a), "-i", str(b), "-lavfi",
                          "[0:v]format=yuv420p10le[x];[1:v]format=yuv420p10le[y];[x][y]psnr", "-f", "null", "-"],
                         capture_output=True, text=True).stderr
    m = re.search(r"PSNR y:([\d.]+) u:([\d.]+) v:([\d.]+)", log)
    return tuple(float(v) for v in m.groups()) if m else None


def first_event_seconds(path, stream):
    """Time of the flash (video) or beep (audio)."""
    if stream == "v":
        out = sh("ffprobe", "-v", "error", "-f", "lavfi", "-i", f"movie={path},format=yuv420p10le,signalstats",
                 "-show_entries", "frame=pts_time:frame_tags=lavfi.signalstats.YAVG", "-of", "csv=p=0")
        for line in out.splitlines():
            t, y = line.split(",")[:2]
            if float(y) > 900:  # 10-bit full white
                return float(t)
        return None
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:a:0", "-ac", "1", "-ar", "48000",
                          "-f", "s16le", "-"], capture_output=True).stdout
    import array
    pcm = array.array("h", raw)
    for i, v in enumerate(pcm):
        if abs(v) > 3000:
            return i / 48000
    return None


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    src = OUT / "source.mov"
    if not src.exists():
        make_source(src)

    resolve = connect()
    p = setup_project(resolve, src)

    presets.install(resolve, p)

    results, failures = [], 0

    p.LoadRenderPreset("ProRes 422 HQ")
    _, status = render(p, "reference")
    ref = OUT / "reference.mov"
    print(f"reference ProRes: {status}")

    for name, codec, *_ in presets.PRESETS:
        if not p.LoadRenderPreset(name):
            print(f"FAIL {name}: preset load failed")
            failures += 1
            continue
        label = re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_")
        took, status = render(p, label)
        f = OUT / f"{label}.mp4"
        if status != "Complete" or not f.exists():
            print(f"FAIL {name}: render {status}")
            failures += 1
            continue

        info = probe(f)
        v = next(s for s in info["streams"] if s["codec_type"] == "video")
        a = next((s for s in info["streams"] if s["codec_type"] == "audio"), {})
        want_codec = "hevc" if codec == presets.HEVC10 else "h264"
        want_pix = "yuv420p10le" if want_codec == "hevc" else "yuv420p"
        width = int(v["width"])
        q = psnr(f, ref) if width == 1920 else None
        av = None
        tv, ta = first_event_seconds(f, "v"), first_event_seconds(f, "a")
        if tv is not None and ta is not None:
            av = (ta - tv) * 1000
        checks = {
            "video codec": v["codec_name"] == want_codec,
            "pixel format": v["pix_fmt"] == want_pix,
            "audio AAC stereo 48k": a.get("codec_name") == "aac" and a.get("channels") == 2
                                    and a.get("sample_rate") == "48000",
            "faststart": faststart(f),
            "A/V sync within 1 frame": av is not None and abs(av) <= 1000 / FPS,
            # constant-quality presets must look like the reference; the VBR preset must hit ~12 Mb/s
            "quality": (q is None or q[0] >= 40) if codec == presets.HEVC10
                       else abs(int(v.get("bit_rate", 0)) / 12e6 - 1) < 0.15,
        }
        ok = all(checks.values())
        failures += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'} {name}: {took:.1f}s, {f.stat().st_size / 1e6:.1f} MB, "
              f"{v['codec_name']} {v['pix_fmt']} {width}x{v['height']}, audio {a.get('codec_name')} "
              f"{int(a.get('bit_rate', 0)) // 1000} kb/s, video {int(v.get('bit_rate', 0)) / 1e6:.1f} Mb/s, A/V {av if av is None else round(av, 1)} ms, PSNR {q}")
        for k, good in checks.items():
            if not good:
                print(f"     x {k}")

    print("ALL PASS" if failures == 0 else f"{failures} FAILED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
