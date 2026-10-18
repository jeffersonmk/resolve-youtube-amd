#!/usr/bin/env python3
"""Create the "YouTube AMD" render presets inside a running DaVinci Resolve Studio.

Resolve's scripting API cannot pick a plugin codec by name, but an imported preset can. So we:
  1. import a tiny preset that only selects the plugin codecs (video + AAC audio),
  2. load it, apply resolution / audio / Network Optimization (= MP4 faststart) through the API,
  3. save it as the final named preset and remove the temporary one.

Usage (Resolve must be open, Preferences > System > General > External scripting = Local):
  /usr/bin/python3 scripts/install_presets.py            # create/update the presets
  /usr/bin/python3 scripts/install_presets.py --remove   # delete them
"""
import os
import sys
import tempfile
import time
from pathlib import Path

sys.path.append("/opt/resolve/Developer/Scripting/Modules")
os.environ.setdefault("RESOLVE_SCRIPT_API", "/opt/resolve/Developer/Scripting")
os.environ.setdefault("RESOLVE_SCRIPT_LIB", "/opt/resolve/libs/Fusion/fusionscript.so")

PLUGIN = "87718B121A60422EB4A33E4D33CF6267"
HEVC10 = "DE8915461F5C4684AF614F67B9E2E6C9"
H264 = "5CA4470FB43A43DF938EAF178135EEA8"
AAC = "F36DE6C3F16D4D41880CD5A772E0CDD3"

TEMPLATE = Path(__file__).with_name("preset_template.xml")

# name, video codec, bit depth, width, height
PRESETS = [
    ("YouTube AMD - HEVC 10-bit 1440p", HEVC10, 10, 2560, 1440),
    ("YouTube AMD - HEVC 10-bit 1080p", HEVC10, 10, 1920, 1080),
    ("YouTube AMD - H.264 1080p", H264, 8, 1920, 1080),
]


def connect():
    try:
        import DaVinciResolveScript as dvr
    except ImportError:
        sys.exit("DaVinci Resolve scripting module not found in /opt/resolve.")
    for _ in range(10):
        resolve = dvr.scriptapp("Resolve")
        if resolve:
            return resolve
        time.sleep(1)
    sys.exit("Could not reach DaVinci Resolve. Open it first and set "
             "Preferences > System > General > External scripting using = Local.")


def codec_xml(codec, depth, index):
    xml = TEMPLATE.read_text()
    return (xml.replace("@DBID@", f"6f1d2c3b-0000-4000-8000-{index:012d}")
               .replace("@VCODEC@", f"{PLUGIN}:{codec}")
               .replace("@ACODEC@", f"{PLUGIN}:{AAC}")
               .replace("@DEPTH@", str(depth)))


def remove(project):
    for name, *_ in PRESETS:
        if name in (project.GetRenderPresetList() or []):
            project.DeleteRenderPreset(name)
            print(f"removed: {name}")


def install(resolve, project):
    current = project.GetCurrentRenderFormatAndCodec()
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, codec, depth, width, height) in enumerate(PRESETS):
            temp_name = f"yta_tmp_{i}"
            path = Path(tmp) / f"{temp_name}.xml"
            path.write_text(codec_xml(codec, depth, i))
            if temp_name in (project.GetRenderPresetList() or []):
                project.DeleteRenderPreset(temp_name)
            if not resolve.ImportRenderPreset(str(path)) or not project.LoadRenderPreset(temp_name):
                sys.exit(f"Could not import preset {name}. Is the plugin installed and Resolve restarted?")
            project.SetRenderSettings({
                "FormatWidth": width,
                "FormatHeight": height,
                "ExportVideo": True,
                "ExportAudio": True,
                "AudioBitDepth": 16,
                "AudioSampleRate": 48000,
                "NetworkOptimization": True,
            })
            if name in (project.GetRenderPresetList() or []):
                project.DeleteRenderPreset(name)
            ok = project.SaveAsNewRenderPreset(name)
            project.DeleteRenderPreset(temp_name)
            print(f"{'installed' if ok else 'FAILED'}: {name}")
    if current.get("format") and current.get("codec"):
        project.SetCurrentRenderFormatAndCodec(current["format"], current["codec"])


def main():
    resolve = connect()
    project = resolve.GetProjectManager().GetCurrentProject()
    if not project:
        sys.exit("Open any project in Resolve first.")
    if "--remove" in sys.argv:
        remove(project)
    else:
        install(resolve, project)


if __name__ == "__main__":
    main()
