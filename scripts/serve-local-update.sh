#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# serve-local-update.sh — Local update server for testing the helix-screen
#                          download → install → restart flow end-to-end.
#
# OVERVIEW
# --------
# Builds a platform package, generates a manifest.json with a fixed high test
# version (99.0.0) guaranteed to be newer than any real install, and serves
# both over HTTP. The running helix-screen binary sees the manifest version
# as newer than itself and triggers the full self-update path without
# touching VERSION.txt.
#
# The test version is always 99.0.0 so the manifest is always seen as newer
# regardless of what version is currently installed on the device. Use
# --no-bump to serve the exact VERSION.txt version instead.
#
# Supported platforms (mirrors the *-docker Makefile targets):
#   pi            Raspberry Pi 64-bit aarch64  (default)
#   pi32          Raspberry Pi 32-bit armhf
#   ad5m          FlashForge Adventurer 5M (armv7-a)
#   cc1           Centauri Carbon 1 (armv7-a)
#   k1            Creality K1
#   k1-dynamic    Creality K1 (dynamic linking)
#   k2            Creality K2
#   snapmaker-u1  Snapmaker U1
#
# QUICK START
# -----------
#   # 1. Set your device's address (once, add to ~/.zshrc or ~/.bashrc):
#   export HELIX_TEST_USERNAME=pi
#   export HELIX_TEST_PRINTER=helixscreen.local   # or an IP address
#
#   # 2. First time only — configure the device to use dev channel:
#   ./scripts/serve-local-update.sh --configure-remote
#
#   # 3. Every subsequent test iteration:
#   ./scripts/serve-local-update.sh [--platform PLATFORM]
#
#   helix-screen will check the manifest, see a newer version, download
#   the tarball, run install.sh, and restart itself.
#
#   To test the installer directly on the device (bypassing the update checker):
#   ssh USERNAME@PRINTER 'sh /tmp/install.sh --local /tmp/helixscreen-update.tar.gz'
#   (install.sh is copied to /tmp/ automatically by --configure-remote)
#
# ENVIRONMENT VARIABLES
# ---------------------
#   HELIX_TEST_PRINTER   Device hostname or IP  (default: helixscreen.local)
#   HELIX_TEST_USERNAME  Device SSH username    (default: pi)
#
#   Legacy names still accepted for backward compatibility:
#   HELIX_TEST_PI_HOST → HELIX_TEST_PRINTER
#   HELIX_TEST_PI_USER → HELIX_TEST_USERNAME
#
# OPTIONS
# -------
#   --platform PLATFORM   Target platform to build and serve (default: pi).
#                         See platform list above.
#   --configure-remote    SSH into the device, write dev channel + dev_url into
#                         settings.json, enable HELIX_LOG_LEVEL=debug in
#                         helixscreen.env for debug logging, copy install.sh to
#                         /tmp/ for local install testing, and restart the
#                         helixscreen service. Run once per device (or after a
#                         factory reset). Requires SSH key access.
#                         Note: uses Pi paths (~/helixscreen/). For other
#                         platforms configure settings.json manually.
#   --no-bump             Serve the exact version from VERSION.txt instead of
#                         99.0.0. Useful if you manually set a higher version.
#   --no-build            Skip compile + package. Patches install.sh from the
#                         repo into the existing dist/ tarball so script-only
#                         changes (install.sh, no binary changes) can be tested
#                         immediately.
#   --port PORT           HTTP port to listen on (default: 8765).
#
# DEPENDENCIES
# ------------
#   - Docker (for *-docker cross-compilation targets)
#   - python3 (for the HTTP server and device settings.json patch)
#   - ssh + scp with key for $HELIX_TEST_USERNAME@$HELIX_TEST_PRINTER
#   - rsync (used by package.sh)
#
# NOTES
# -----
#   - The bumped version is only used for the tarball filename and manifest.
#     VERSION.txt is never modified.
#   - After install the device binary still reports VERSION.txt's version
#     (< 99.0.0), so the next run will again offer an update — intentional
#     for iteration.
#   - To reset the device back to the stable channel, re-run --configure-remote
#     after removing the dev_url key, or delete settings.json on the device.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Defaults (overridable via env) ────────────────────────────────────────────
# Accept both new names and legacy PI_* names for backward compatibility
PRINTER="${HELIX_TEST_PRINTER:-${HELIX_TEST_PI_HOST:-helixscreen.local}}"
USERNAME="${HELIX_TEST_USERNAME:-${HELIX_TEST_PI_USER:-pi}}"
PORT=8765
BUILD=1
BUMP=1
CONFIGURE_REMOTE=0
PLATFORM=pi

VALID_PLATFORMS="pi pi32 ad5m cc1 k1 k1-dynamic k2 snapmaker-u1"

# ── Args ──────────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --configure-remote) CONFIGURE_REMOTE=1; shift ;;
        --no-build)         BUILD=0; shift ;;
        --no-bump)          BUMP=0; shift ;;
        --platform)         PLATFORM=$2; shift 2 ;;
        --port)             PORT=$2; shift 2 ;;
        --help|-h)
            sed -n '2,/^set -/p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        pi|pi32|ad5m|cc1|k1|k1-dynamic|k2|snapmaker-u1) PLATFORM=$1; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Validate platform ─────────────────────────────────────────────────────────
if ! echo "$VALID_PLATFORMS" | grep -qw "$PLATFORM"; then
    echo "ERROR: Unknown platform '$PLATFORM'"
    echo "  Valid platforms: $VALID_PLATFORMS"
    exit 1
fi

# ── Dependency checks ─────────────────────────────────────────────────────────
_missing=()
command -v python3 >/dev/null || _missing+=(python3)
if [[ $BUILD -eq 1 ]]; then
    command -v docker >/dev/null || _missing+=(docker)
fi
if [[ $CONFIGURE_REMOTE -eq 1 ]]; then
    command -v ssh >/dev/null || _missing+=("ssh")
    command -v scp >/dev/null || _missing+=("scp")
fi
if [[ ${#_missing[@]} -gt 0 ]]; then
    echo "ERROR: Missing required dependencies: ${_missing[*]}"
    exit 1
fi

# ── Version ───────────────────────────────────────────────────────────────────
BASE_VERSION="$(tr -d '[:space:]' < "$PROJECT_DIR/VERSION.txt")"  # e.g. 0.10.4

# Use a fixed high test version so the manifest is always newer than any real
# install, regardless of which branch or version is currently on the device.
if [[ $BUMP -eq 1 ]]; then
    TEST_VERSION="99.0.0"
else
    TEST_VERSION="$BASE_VERSION"
fi

# install.sh prepends "v" to the manifest version, so the manifest must use
# bare version numbers (e.g. "99.0.0") to avoid "vv99.0.0" in the installer.
VERSION_BARE="${TEST_VERSION}"
VERSION="v${TEST_VERSION}"
TARBALL_NAME="helixscreen-${PLATFORM}-${VERSION}.tar.gz"
TARBALL_PATH="$PROJECT_DIR/dist/$TARBALL_NAME"

# ── Local IP ──────────────────────────────────────────────────────────────────
LOCAL_IP=$(ipconfig getifaddr en0 2>/dev/null || \
           ip route get 1 2>/dev/null | awk '{print $7; exit}' || \
           echo "127.0.0.1")
BASE_URL="http://${LOCAL_IP}:${PORT}"

# ── Port availability check ────────────────────────────────────────────────────
if lsof -iTCP:"$PORT" -sTCP:LISTEN -t >/dev/null 2>&1; then
    echo "ERROR: Port $PORT is already in use."
    echo "  Kill the existing process or use --port to choose another port."
    exit 1
fi

# ── SSH reachability check (only for --configure-remote) ──────────────────────
if [[ $CONFIGURE_REMOTE -eq 1 ]]; then
    echo "[serve-local-update] Checking SSH connectivity to ${USERNAME}@${PRINTER}..."
    if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "${USERNAME}@${PRINTER}" true 2>/dev/null; then
        echo ""
        echo "ERROR: Cannot reach ${USERNAME}@${PRINTER} via SSH."
        echo "  Check that the device is powered on and SSH key auth is configured."
        exit 1
    fi
    echo "  OK"
fi

echo ""
echo "  serve-local-update"
echo "  ══════════════════"
echo "  Platform     : $PLATFORM"
echo "  Base version : $BASE_VERSION  (VERSION.txt — unchanged)"
echo "  Test version : $TEST_VERSION  $([ $BUMP -eq 1 ] && echo "(fixed high — always newer than installed)" || echo "(no bump)")"
echo "  Tarball      : $TARBALL_NAME"
echo "  Serving at   : $BASE_URL"
echo "  Device       : ${USERNAME}@${PRINTER}"
echo ""

# ── Bundle install.sh from modules ───────────────────────────────────────────
# install.sh is auto-generated from scripts/lib/installer/*.sh by bundle-installer.sh.
# Always regenerate before build or patch so the tarball contains the latest installer,
# even if only module sources were edited (not the bundled install.sh).
echo "[serve-local-update] Bundling install.sh from modules..."
"$SCRIPT_DIR/bundle-installer.sh" -o "$PROJECT_DIR/scripts/install.sh"
echo ""

# ── Build ─────────────────────────────────────────────────────────────────────
if [[ $BUILD -eq 1 ]]; then
    echo "[serve-local-update] Compiling ${PLATFORM} binary (docker)..."
    make -C "$PROJECT_DIR" "${PLATFORM}-docker"
    echo ""
    echo "[serve-local-update] Packaging..."
    "$SCRIPT_DIR/package.sh" "${PLATFORM}" --version "$VERSION"
    echo ""
fi

if [[ ! -f "$TARBALL_PATH" ]]; then
    echo "ERROR: Tarball not found: $TARBALL_PATH"
    echo ""
    echo "  Expected: $TARBALL_PATH"
    echo "  Either run without --no-build, or build manually:"
    echo "    make release-${PLATFORM}"
    exit 1
fi

# ── Patch install.sh into existing tarball (--no-build fast path) ─────────────
if [[ $BUILD -eq 0 ]]; then
    echo "[serve-local-update] Patching install.sh into tarball..."
    PATCH_DIR="$(mktemp -d)"
    trap 'rm -rf "$PATCH_DIR"' EXIT
    tar -xzf "$TARBALL_PATH" -C "$PATCH_DIR"
    if [[ ! -d "$PATCH_DIR/helixscreen" ]]; then
        echo "ERROR: Tarball does not contain expected helixscreen/ directory."
        exit 1
    fi
    cp "$PROJECT_DIR/scripts/install.sh" "$PATCH_DIR/helixscreen/install.sh"
    chmod +x "$PATCH_DIR/helixscreen/install.sh"
    COPYFILE_DISABLE=1 tar -czf "$TARBALL_PATH" --owner=0 --group=0 -C "$PATCH_DIR" helixscreen
    rm -rf "$PATCH_DIR"
    trap - EXIT
    echo "[serve-local-update] install.sh patched."
    echo ""
fi

# ── Manifest ──────────────────────────────────────────────────────────────────
MANIFEST_PATH="$PROJECT_DIR/dist/manifest.json"
TARBALL_SHA256=$(shasum -a 256 "$TARBALL_PATH" | awk '{print $1}')
TARBALL_SIZE=$(stat -f%z "$TARBALL_PATH" 2>/dev/null || stat -c%s "$TARBALL_PATH" 2>/dev/null)
cat > "$MANIFEST_PATH" <<EOF
{
  "version": "${VERSION_BARE}",
  "channel": "dev",
  "assets": {
    "${PLATFORM}": {
      "filename": "${TARBALL_NAME}",
      "url": "${BASE_URL}/${TARBALL_NAME}",
      "sha256": "${TARBALL_SHA256}",
      "size": ${TARBALL_SIZE}
    }
  }
}
EOF
echo "[serve-local-update] Manifest → $MANIFEST_PATH"
echo "  SHA256: ${TARBALL_SHA256}"
echo "  Size:   ${TARBALL_SIZE} bytes"

# ── Configure remote device ────────────────────────────────────────────────────
if [[ $CONFIGURE_REMOTE -eq 1 ]]; then
    echo "[serve-local-update] Configuring ${USERNAME}@${PRINTER} ..."
    ssh "${USERNAME}@${PRINTER}" "python3 -c \"
import json, os, re

# Write dev channel + dev_url into settings.json (read by Config::get_instance())
path = os.path.expanduser('~/helixscreen/config/settings.json')
if not os.path.exists(path):
    # Try legacy name
    path = os.path.expanduser('~/helixscreen/config/helixconfig.json')
    if not os.path.exists(path):
        print('  ERROR: settings.json not found at ~/helixscreen/config/')
        print('  Run the HelixScreen setup wizard first, then re-run --configure-remote.')
        raise SystemExit(1)
with open(path) as f:
    data = json.load(f)
data.setdefault('update', {})
data['update']['channel'] = 2
data['update']['dev_url'] = '${BASE_URL}/'
with open(path, 'w') as f:
    json.dump(data, f, indent=2)
print('  update/channel =', data['update']['channel'], '(dev)')
print('  update/dev_url =', data['update']['dev_url'])

# Enable HELIX_LOG_LEVEL=debug in helixscreen.env (enables debug logging via launcher).
# Launcher checks INSTALL_DIR/config/ first, then /etc/helixscreen/.
DEFAULTS = '''# HelixScreen environment configuration
# See config/helixscreen.env in the repo for full documentation.
MOONRAKER_HOST=localhost
MOONRAKER_PORT=7125
HELIX_LOG_LEVEL=info
'''
env_candidates = [
    os.path.expanduser('~/helixscreen/config/helixscreen.env'),
    '/etc/helixscreen/helixscreen.env',
]
env_path = next((p for p in env_candidates if os.path.exists(p)), None)
if env_path is None:
    env_path = env_candidates[0]
    os.makedirs(os.path.dirname(env_path), exist_ok=True)
    content = DEFAULTS
    print('  Created', env_path, 'with defaults')
else:
    content = open(env_path).read()
    # If the file is missing essential defaults (was corrupted by a previous run),
    # restore them before setting HELIX_LOG_LEVEL.
    if 'MOONRAKER_HOST' not in content:
        content = DEFAULTS + content
        print('  Restored missing defaults in', env_path)
# Set HELIX_LOG_LEVEL=debug (remove any existing line first, then append)
content = re.sub(r'^#?\s*HELIX_LOG_LEVEL=.*\n?', '', content, flags=re.MULTILINE)
content = re.sub(r'^#?\s*HELIX_DEBUG=.*\n?', '', content, flags=re.MULTILINE)
content = content.rstrip('\n') + '\nHELIX_LOG_LEVEL=debug\n'
open(env_path, 'w').write(content)
print('  HELIX_LOG_LEVEL=debug  (debug logging enabled in', env_path + ')')
\""
    echo ""
    echo "[serve-local-update] Copying install.sh to /tmp/ on ${USERNAME}@${PRINTER} ..."
    scp "$PROJECT_DIR/scripts/install.sh" "${USERNAME}@${PRINTER}:/tmp/install.sh"
    echo "  To test the installer directly on the device (bypasses update checker):"
    echo "    ssh ${USERNAME}@${PRINTER} 'sh /tmp/install.sh --local /tmp/helixscreen-update.tar.gz'"
    echo ""
    echo "[serve-local-update] Restarting helix-screen on ${USERNAME}@${PRINTER} ..."
    ssh "${USERNAME}@${PRINTER}" "sudo systemctl restart helixscreen"
    echo "[serve-local-update] helix-screen restarted."
    echo "  To revert: remove update/dev_url from ~/helixscreen/config/settings.json on the device."
    echo ""
fi

# ── Serve ─────────────────────────────────────────────────────────────────────
echo ""
echo "  Ready"
echo "  ─────────────────────────────────────────────────"
echo "  Manifest : ${BASE_URL}/manifest.json"
echo "  Tarball  : ${BASE_URL}/${TARBALL_NAME}"
echo ""
echo "  Device expects (set via --configure-remote if not already done):"
echo "    update/channel  = 2"
echo "    update/dev_url  = \"${BASE_URL}/\""
echo ""
echo "  Press Ctrl+C to stop."
echo "  ─────────────────────────────────────────────────"
echo ""

cd "$PROJECT_DIR/dist"
python3 -m http.server "$PORT"
