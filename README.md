# YouTube AMD — DaVinci Resolve Studio encoder plugin

A small encoder plugin for **DaVinci Resolve Studio on Linux** that renders
YouTube-ready MP4 files on the **AMD GPU** (VAAPI), with **AAC audio** — something
Resolve on Linux cannot do by itself.

> **Note:** I made this plugin to speed up my own workflow (Ryzen 5 5600 + Radeon RX 9060 XT,
> CachyOS, Resolve Studio 21.1). I don't guarantee it works well on other machines.

## What you get

Three presets in the **Deliver** page:

| Preset | Video | Audio | Use it for |
|---|---|---|---|
| **YouTube AMD - HEVC 10-bit 1440p** ⭐ | H.265 10-bit, constant quality QP 20, 2560×1440 | AAC 320 kb/s | Best YouTube quality (YouTube gives 1440p uploads a better encode) |
| YouTube AMD - HEVC 10-bit 1080p | H.265 10-bit, QP 20, 1920×1080 | AAC 320 kb/s | Smaller file, still high quality |
| YouTube AMD - H.264 1080p | H.264 8-bit, VBR 12 Mb/s (max 18, buffer 24) | AAC 320 kb/s | Most compatible file |

The H.264 preset does the same thing as this ffmpeg command:

```bash
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mov -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -rc_mode VBR -b:v 12M -maxrate 18M -bufsize 24M \
  -c:a aac -b:a 320k -movflags +faststart output.mp4
```

Every file comes out with **faststart** (Resolve calls this *Network Optimization*), so it starts
uploading/playing right away.

Settings you can still change in Deliver → Video, after picking the codec:
**Encoder Preset** (Speed / Balanced / Quality), **Rate Control** (Constant Quality or Variable
Bitrate), **QP**, **Bit Rate**, **Max Bit Rate**, **Buffer Size**, and a **Reset to YouTube defaults**
button. Audio: **Bit Rate** 192 / 256 / 320 kb/s.

## Requirements

- DaVinci Resolve **Studio** (the free version does not load plugins), installed in `/opt/resolve`
- An AMD GPU with working VAAPI (`vainfo` lists `VAProfileHEVCMain10 : VAEntrypointEncSlice`)
- Build tools and FFmpeg. On Arch / CachyOS:

```bash
sudo pacman -S --needed base-devel cmake ninja pkgconf ffmpeg libva
```

## Install

1. Open a terminal and download the project (or unzip it) into `~/Projects`:

   ```bash
   cd ~/Projects
   git clone https://github.com/jeffersonmk/resolve-youtube-amd.git
   cd resolve-youtube-amd
   ```

2. Build and install the plugin (it may ask for your password):

   ```bash
   ./scripts/install.sh
   ```

3. **Close and reopen** DaVinci Resolve Studio. Open any project.

4. Allow scripts to talk to Resolve, once:
   **DaVinci Resolve → Preferences → System → General → External scripting using → Local**.

5. Create the presets:

   ```bash
   /usr/bin/python3 scripts/install_presets.py
   ```

   You should see three `installed:` lines.

## Use

1. Go to the **Deliver** page.
2. At the top of the *Render Settings* panel, click the **⋯** next to the presets and pick
   **YouTube AMD - HEVC 10-bit 1440p** (or another one from the table).
3. Choose the file name and folder, **Add to Render Queue**, **Render All**.

To set it up by hand instead: Format **MP4** → Video codec **YouTube HEVC 10-bit (AMD VAAPI)** or
**YouTube H.264 8-bit (AMD VAAPI)** → Audio codec **AAC Stereo (YouTube AMD)** → Advanced Settings →
tick **Network Optimization**.

## Update

```bash
cd ~/Projects/resolve-youtube-amd
git pull
./scripts/install.sh
```

Then restart Resolve. Run `./scripts/install.sh` again also **after updating Resolve or FFmpeg**:
the plugin is built against your system FFmpeg, and a new FFmpeg version needs a rebuild.
The presets don't need to be created again.

## Uninstall

1. Keep Resolve open (so the presets can be removed too) and run:

   ```bash
   cd ~/Projects/resolve-youtube-amd
   ./scripts/uninstall.sh
   ```

2. Restart Resolve. Optionally delete the folder: `rm -rf ~/Projects/resolve-youtube-amd`.

If Resolve was closed, the presets stay; delete the three **YouTube AMD - …** presets in Deliver
(right-click → Delete), or open Resolve and run `/usr/bin/python3 scripts/install_presets.py --remove`.

## Using it together with the AUR FFmpeg plugin

This plugin can be installed side by side with
[`davinci-ffmpeg-encoder-plugin`](https://aur.archlinux.org/packages/davinci-ffmpeg-encoder-plugin)
(EdvinNilsson's FFmpeg plugin). Nothing to configure: they use different IDs and different
folders in `/opt/resolve/IOPlugins`, and updating or removing one doesn't touch the other.

In Deliver you will see both sets of codecs. Ours are the ones named **YouTube … (AMD VAAPI)**
and **AAC Stereo (YouTube AMD)**; the AUR ones end in **(FFmpeg)**. The *YouTube AMD* presets
always use ours. You can also mix them: an AUR video codec with our AAC audio works.

Tested together on my machine (Resolve Studio 21.1.1, AUR plugin 1.4.0): all presets pass and
a render with the AUR HEVC codec + our AAC is fine.

**Resolve closes right when it starts (only with the AUR plugin installed):** the AUR plugin
starts Vulkan when Resolve loads, and some Vulkan layers crash there — on my machine it was
**lsfg-vk** (Lossless Scaling frame generation). Our plugin doesn't use Vulkan, so it isn't
affected. Fix: start Resolve with that layer turned off, e.g. in
`~/.local/share/applications/com.blackmagicdesign.resolve.desktop`:

```
Exec=env DISABLE_LSFGVK=1 /opt/resolve/bin/resolve %u
```

## Troubleshooting

| Problem | Fix |
|---|---|
| Codecs don't appear in Deliver | Pick Format **MP4** or **QuickTime**. Check the log: `grep "YouTube AMD" ~/.local/share/DaVinciResolve/logs/ResolveDebug.txt` — it says *registered* for each codec or why it was skipped. |
| `install_presets.py` can't reach Resolve | Resolve must be open with a project, and *External scripting using* must be **Local**. |
| Render fails right away after an FFmpeg update | Run `./scripts/install.sh` again and restart Resolve. |
| Resolve's *Estimated file size* is huge | Ignore it — Resolve can't estimate plugin codecs. Render 1 minute and look at the real file. |

## How it was tested

`tests/selftest.py` drives a headless Resolve (`/opt/resolve/bin/resolve -nogui`), renders a
10-second test clip with each preset and a ProRes reference, and checks codec, 10-bit, AAC stereo
48 kHz, faststart, audio/video sync (a flash and a beep at the same frame) and PSNR against the
reference. Last result on my machine: all three presets pass, sync error 0.0 ms, about 2–3 s
per 10 s clip.

## Credits and license

GPL-3.0. The Resolve ↔ FFmpeg plumbing is adapted from
[EdvinNilsson/ffmpeg_encoder_plugin](https://github.com/EdvinNilsson/ffmpeg_encoder_plugin) (GPL-3.0);
the AAC approach learned from
[Toxblh/davinci-linux-aac-codec](https://github.com/Toxblh/davinci-linux-aac-codec) and
[hexitnz/Resolve-Linux-Studio-AAC-FDK-Encoder-plugin](https://github.com/hexitnz/Resolve-Linux-Studio-AAC-FDK-Encoder-plugin).
The `src/include` and `src/wrapper` files come from Blackmagic Design's IO Encode Plugin SDK.
