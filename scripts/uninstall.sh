#!/usr/bin/env bash
# Remove the "YouTube AMD" plugin (and, if Resolve is open, its render presets).
set -euo pipefail

cd "$(dirname "$0")/.."
RESOLVE_DIR="${RESOLVE_DIR:-/opt/resolve}"
BUNDLE="$RESOLVE_DIR/IOPlugins/youtube_amd_plugin.dvcp.bundle"

if pgrep -x resolve >/dev/null; then
    echo "==> Removing render presets from the open Resolve"
    /usr/bin/python3 scripts/install_presets.py --remove || echo "(could not reach Resolve; delete the 'YouTube AMD - ...' presets in Deliver by hand)"
else
    echo "==> Resolve is closed: presets were kept. To remove them, open Resolve and run:"
    echo "      /usr/bin/python3 scripts/install_presets.py --remove"
    echo "    (or delete the 'YouTube AMD - ...' presets in Deliver by hand)"
fi

if [ -d "$BUNDLE" ]; then
    echo "==> Removing $BUNDLE"
    if [ -w "$(dirname "$BUNDLE")" ]; then rm -rf "$BUNDLE"; else sudo rm -rf "$BUNDLE"; fi
else
    echo "==> Plugin was not installed"
fi

rm -rf build
echo "==> Done. Restart DaVinci Resolve."
