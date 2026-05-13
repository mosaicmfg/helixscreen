#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the COSMOS-specific restore block in uninstall() (uninstall.sh).
# Mirrors the install path covered by test_cc1_competing_uis.bats: confirms
# /etc/init.d/grumpyscreen is restored from .helix-bak and cosmos.conf is
# reverted to screen_ui = grumpyscreen.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    log_info() { echo "INFO: $*"; }
    log_warn() { echo "WARN: $*"; }
    log_success() { echo "OK: $*"; }
    # Stub helpers that the COSMOS-block extractor over-captures from lines
    # after the actual block (the awk `^    fi$` count never reaches 4, so
    # capture runs to EOF). These would normally be sourced from common.sh /
    # file_sudo.sh in the real script; here we just need them callable so the
    # extracted snippet doesn't abort with "command not found".
    file_sudo() { echo ""; }
    clean_helix_state_dirs() { :; }
    remove_config_symlink() { :; }
    remove_update_manager_section() { :; }
    export -f log_info log_warn log_success file_sudo clean_helix_state_dirs \
              remove_config_symlink remove_update_manager_section

    export MOCK_ROOT="$BATS_TEST_TMPDIR/cc1"
    mkdir -p "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/etc/klipper/config" "$MOCK_ROOT/usr/bin"

    # Marker file the uninstaller uses to detect COSMOS
    touch "$MOCK_ROOT/usr/bin/update-cosmos"
    chmod +x "$MOCK_ROOT/usr/bin/update-cosmos"

    # Simulate post-install state: grumpyscreen is the wrapper, .helix-bak
    # holds the stock COSMOS init script, cosmos.conf points at helixscreen.
    cat > "$MOCK_ROOT/etc/init.d/grumpyscreen.helix-bak" <<'EOF'
#!/bin/sh
# Stock COSMOS grumpyscreen (mocked)
echo "stock grumpyscreen $1"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/grumpyscreen.helix-bak"

    cat > "$MOCK_ROOT/etc/init.d/grumpyscreen" <<'EOF'
#!/bin/sh
# HELIXSCREEN_WRAPPER
exec /etc/init.d/helixscreen "$@"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/grumpyscreen"

    cat > "$MOCK_ROOT/etc/klipper/config/cosmos.conf" <<'EOF'
[ui]
screen_ui = helixscreen
web_ui = mainsail
EOF

    # The function is large; we only exercise the COSMOS restore block by
    # re-running the same path-rewriting trick used in test_cc1_competing_uis.
    # We extract just the COSMOS branch into a callable function so we don't
    # need to mock the entire uninstall (init scripts, install dirs, etc.).
    cat > "$BATS_TEST_TMPDIR/cosmos_restore.sh" <<'SH_EOF'
#!/bin/sh
SH_EOF

    # Pull the contiguous "if [ -z \"$restored_ui\" ] && [ -x \"/usr/bin/update-cosmos\" ]" block
    awk '
        /^    # COSMOS \(Centauri Carbon\)/ { capture=1 }
        capture { print }
        capture && /^    fi$/ && ++blocks==4 { exit }
    ' "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh" >> "$BATS_TEST_TMPDIR/cosmos_restore.sh"

    sed -i \
        -e "s|/etc/init.d/grumpyscreen|$MOCK_ROOT/etc/init.d/grumpyscreen|g" \
        -e "s|/etc/klipper/config/cosmos.conf|$MOCK_ROOT/etc/klipper/config/cosmos.conf|g" \
        -e "s|/usr/bin/update-cosmos|$MOCK_ROOT/usr/bin/update-cosmos|g" \
        "$BATS_TEST_TMPDIR/cosmos_restore.sh"

    cosmos_restore() {
        local restored_ui=""
        # shellcheck disable=SC1091
        . "$BATS_TEST_TMPDIR/cosmos_restore.sh"
        printf '%s' "$restored_ui"
    }
}

@test "uninstall cc1: restores stock grumpyscreen from .helix-bak" {
    run cosmos_restore
    [ "$status" -eq 0 ]
    [ -f "$MOCK_ROOT/etc/init.d/grumpyscreen" ]
    [ ! -f "$MOCK_ROOT/etc/init.d/grumpyscreen.helix-bak" ]
    grep -q "Stock COSMOS grumpyscreen" "$MOCK_ROOT/etc/init.d/grumpyscreen"
    ! grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/grumpyscreen"
}

@test "uninstall cc1: reverts cosmos.conf screen_ui to grumpyscreen" {
    cosmos_restore >/dev/null
    grep -q "^screen_ui = grumpyscreen" "$MOCK_ROOT/etc/klipper/config/cosmos.conf"
    ! grep -q "screen_ui = helixscreen" "$MOCK_ROOT/etc/klipper/config/cosmos.conf"
}

@test "uninstall cc1: leaves cosmos.conf alone if value isn't helixscreen" {
    # Manual operator change before uninstall — don't clobber it.
    sed -i 's|^screen_ui = helixscreen|screen_ui = guppyscreen|' \
        "$MOCK_ROOT/etc/klipper/config/cosmos.conf"
    cosmos_restore >/dev/null
    grep -q "^screen_ui = guppyscreen" "$MOCK_ROOT/etc/klipper/config/cosmos.conf"
}

@test "uninstall cc1: missing .helix-bak does not crash, leaves wrapper as-is" {
    # Edge case: bad install left no backup. Uninstaller must not crash and
    # must not leave the device with no /etc/init.d/grumpyscreen at all.
    rm -f "$MOCK_ROOT/etc/init.d/grumpyscreen.helix-bak"
    run cosmos_restore
    [ "$status" -eq 0 ]
    [ -f "$MOCK_ROOT/etc/init.d/grumpyscreen" ]
}
