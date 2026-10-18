#!/usr/bin/env bash
# Build and install the "YouTube AMD" encoder plugin for DaVinci Resolve Studio (Linux).
# Run again at any time to update (after a git pull, a Resolve update or an FFmpeg update).
set -euo pipefail

cd "$(dirname "$0")/.."
REPO="$PWD"
RESOLVE_DIR="${RESOLVE_DIR:-/opt/resolve}"
BUNDLE="$RESOLVE_DIR/IOPlugins/youtube_amd_plugin.dvcp.bundle"
TARGET="$BUNDLE/Contents/Linux-x86-64"

say() { printf '\n==> %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

say "Checking requirements"
[ -x "$RESOLVE_DIR/bin/resolve" ] || die "DaVinci Resolve not found in $RESOLVE_DIR"
for cmd in cmake c++ pkg-config ffmpeg; do
    command -v "$cmd" >/dev/null || die "'$cmd' is missing. On Arch/CachyOS: sudo pacman -S --needed cmake base-devel pkgconf ffmpeg libva"
done
pkg-config --exists libavcodec libavutil libva || die "FFmpeg/libva development files missing (sudo pacman -S --needed ffmpeg libva)"
ENCODERS="$(ffmpeg -hide_banner -encoders 2>/dev/null || true)"
grep -q hevc_vaapi <<<"$ENCODERS" || die "Your FFmpeg has no hevc_vaapi encoder"
ls /dev/dri/renderD* >/dev/null 2>&1 || die "No GPU render device (/dev/dri/renderD*) found"

say "Building"
GENERATOR=()
command -v ninja >/dev/null && GENERATOR=(-G Ninja)
cmake -S "$REPO" -B "$REPO/build" -DCMAKE_BUILD_TYPE=Release "${GENERATOR[@]}" >/dev/null
cmake --build "$REPO/build"

say "Installing to $TARGET"
SUDO=""
if ! mkdir -p "$TARGET" 2>/dev/null || [ ! -w "$TARGET" ]; then
    SUDO="sudo"
    echo "(administrator password needed to write into $RESOLVE_DIR)"
fi
$SUDO mkdir -p "$TARGET"
$SUDO install -m 755 "$REPO/build/youtube_amd_plugin.dvcp" "$TARGET/youtube_amd_plugin.dvcp"

say "Done"
cat <<'EOF'
Next steps:
  1. Close and reopen DaVinci Resolve Studio.
  2. Create the render presets (Resolve must be open with any project):
       /usr/bin/python3 scripts/install_presets.py
     If it says it cannot reach Resolve: Preferences > System > General >
     "External scripting using" = Local, then run it again.
  3. In Deliver, pick "YouTube AMD - HEVC 10-bit 1440p" from the presets row.
EOF
