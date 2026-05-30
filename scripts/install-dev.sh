#!/bin/sh
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen Installer (Development/Modular Version)
#
# This script requires the lib/installer/ modules and must be run from a repo checkout.
# For end-user installation via curl, use scripts/install.sh instead.
#
# Usage (from repo root):
#   ./scripts/install-dev.sh [options]
#
# Options:
#   --update    Update existing installation (preserves config)
#   --uninstall Remove HelixScreen
#   --clean     Remove old installation completely before installing (no config backup)
#   --version   Specify version (default: latest)
#

# Fail fast on any error
set -eu

# Configuration
GITHUB_REPO="prestonbrown/helixscreen"
SERVICE_NAME="helixscreen"

# Source modules (if running from repo, not bundled)
if [ -z "${_HELIX_BUNDLED_INSTALLER:-}" ]; then
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    LIB_DIR="$SCRIPT_DIR/lib/installer"

    . "$LIB_DIR/common.sh"
    . "$LIB_DIR/platform.sh"
    . "$LIB_DIR/permissions.sh"
    . "$LIB_DIR/requirements.sh"
    . "$LIB_DIR/forgex.sh"
    . "$LIB_DIR/competing_uis.sh"
    . "$LIB_DIR/release.sh"
    . "$LIB_DIR/service.sh"
    . "$LIB_DIR/audio.sh"
    . "$LIB_DIR/moonraker.sh"
    . "$LIB_DIR/recovery.sh"
    . "$LIB_DIR/kiauh.sh"
    . "$LIB_DIR/uninstall.sh"  # uses functions from other modules
    . "$LIB_DIR/main.sh"       # must be last - defines main() that calls everything
fi

# Run main
main "$@"
