#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Snapmaker U1 (Extended Firmware)
#
# The Snapmaker U1 runs Debian Trixie with its own touchscreen UI application.
# HelixScreen uses the DRM backend for double-buffered page flipping.
#
# DRM CRTC keepalive: The U1's display is driven by a Rockchip DRM/KMS
# pipeline (rockchipdrmfb). When the stock UI process exits, the kernel's
# VOP2 driver disables the CRTC, leaving the display permanently black.
# To prevent this, we spawn a background process that holds /dev/dri/card0
# open while we kill the stock UI. Once HelixScreen opens the DRM device
# itself, the keepalive process exits — but the CRTC stays active because
# HelixScreen now has its own fd on the device.
#
# Stock UI: /usr/bin/gui (started by S99screen init script)
# Camera supervisor: /usr/bin/lmd (started by /etc/init.d/S99v4l2-mpp-mipi at
#   boot — and *only* at boot; no fake-service auto-restart). lmd forks
#   /usr/bin/unisrv which subscribes to MQTT topic camera/request and answers
#   Klipper's TIMELAPSE_START / camera.take_a_photo RPCs.
# Touch: TLSC6x capacitive controller (tlsc6x_touch on /dev/input/event0)
# Display: 480x320 32bpp rockchipdrmfb (/dev/fb0)
# SSH access: root@<ip> (password: snapmaker) via extended firmware

# PID of the background keepalive process
DRM_KEEPALIVE_PID=""

# Stop Snapmaker's stock touchscreen UI so HelixScreen can access the display.
#
# CRITICAL: We must hold /dev/dri/card0 open before killing the stock UI.
# Without this, the VOP2 driver disables the CRTC and the display goes
# permanently black until reboot.
platform_stop_competing_uis() {
    # Spawn a background process that holds the DRM device open.
    # It stays alive until helix-screen opens /dev/dri/card0 itself (detected
    # via /proc/*/fd), or for a maximum of 30 seconds as a safety timeout.
    if [ -e /dev/dri/card0 ]; then
        (
            # Hold the device open via our own fd
            exec 3>/dev/dri/card0
            echo "DRM keepalive: holding /dev/dri/card0 (pid $$)"
            # Wait until helix-screen has the device open, or timeout
            elapsed=0
            while [ "$elapsed" -lt 30 ]; do
                for pid_dir in /proc/[0-9]*/fd; do
                    pid=$(echo "$pid_dir" | sed 's|/proc/\([0-9]*\)/fd|\1|')
                    comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
                    case "$comm" in
                        helix-screen)
                            if readlink "$pid_dir"/* 2>/dev/null | grep -q '/dev/dri/card0'; then
                                echo "DRM keepalive: helix-screen (pid $pid) has /dev/dri/card0, releasing"
                                exit 0
                            fi
                            ;;
                    esac
                done
                sleep 1
                elapsed=$((elapsed + 1))
            done
            echo "DRM keepalive: timeout after 30s, releasing"
        ) &
        DRM_KEEPALIVE_PID=$!
        echo "DRM keepalive: background process PID $DRM_KEEPALIVE_PID"
    fi

    # Kill stock UI processes only. unisrv (proprietary camera/MQTT daemon
    # handling Klipper's TIMELAPSE_START) and lmd (its supervisor) are NOT UIs
    # and must keep running for timelapse to work. Killing lmd permanently
    # disables camera RPC until reboot — lmd has no supervisor of its own and
    # /etc/init.d/S99v4l2-mpp-mipi only starts it at boot.
    # NOTE: Do NOT call /etc/init.d/S99screen stop — on the U1, S99screen is
    # patched to delegate to helixscreen.init, which would cause infinite recursion.
    for ui in gui snapmaker-ui snapmaker-screen KlipperScreen klipperscreen; do
        if command -v killall >/dev/null 2>&1; then
            killall "$ui" 2>/dev/null || true
        else
            for pid in $(pidof "$ui" 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
        fi
    done

    # Kill python-based KlipperScreen if running
    # shellcheck disable=SC2009
    for pid in $(ps aux 2>/dev/null | grep -E 'python.*screen\.py' | grep -v grep | awk '{print $2}'); do
        echo "Killing KlipperScreen python process (PID $pid)"
        kill "$pid" 2>/dev/null || true
    done

    # Brief pause to let processes settle
    sleep 1
}

# The U1 display backlight is managed by the kernel/hardware.
platform_enable_backlight() {
    return 0
}

# Debian Trixie manages services via systemd - Klipper/Moonraker should be
# available by the time HelixScreen starts.
platform_wait_for_services() {
    return 0
}

# Ensure lmd (camera/MQTT supervisor) is running. lmd is started once at boot
# by /etc/init.d/S99v4l2-mpp-mipi with no auto-restart wrapper, so if it ever
# dies — historically because older helixscreen builds killed it in
# platform_stop_competing_uis — Klipper's TIMELAPSE_START hits a 5-second MQTT
# timeout and pauses the print. We re-launch lmd with the same imposter env
# the stock init uses; lmd then forks unisrv as its child.
ensure_lmd_running() {
    if pidof lmd >/dev/null 2>&1; then
        return 0
    fi
    if [ ! -x /usr/bin/lmd ]; then
        return 0
    fi
    if [ ! -e /tmp/capture-mipi-raw.sock ]; then
        # Capture pipeline not up yet; lmd would just fail to attach. Let
        # S99v4l2-mpp-mipi finish its sequence on its own.
        return 0
    fi
    echo "lmd not running — restarting camera supervisor"
    rm -f /var/run/unisrv.pid
    (
        export LD_PRELOAD=/usr/local/lib/libv4l2-imposter.so
        export V4L2_IMPOSTER_SOCKET_PATH=/tmp/capture-mipi-raw.sock
        export V4L2_IMPOSTER_DEVICE=/dev/video11
        export V4L2_IMPOSTER_WIDTH=1920
        export V4L2_IMPOSTER_HEIGHT=1080
        export V4L2_IMPOSTER_FORMAT=nv12
        start-stop-daemon -S -b -q -m -p /var/run/unisrv.pid -x /usr/bin/lmd
    )
}

platform_pre_start() {
    export HELIX_CACHE_DIR="/userdata/helixscreen/cache"
    # Force DRM device — skip auto-detection which may race with connector state
    export HELIX_DRM_DEVICE="/dev/dri/card0"

    # Recover the camera supervisor if a prior helixscreen build killed it.
    # Idempotent: no-op when lmd is already alive.
    ensure_lmd_running

    # Restore saved WiFi credentials into wpa_supplicant.
    # The stock Snapmaker app saves WiFi config to /oem/printer_data/gui/
    # and loads it into wpa_supplicant at runtime. Since we replaced the stock
    # app, we need to do this ourselves.
    SAVED_WPA="/oem/printer_data/gui/wpa_supplicant.conf"
    if [ -f "$SAVED_WPA" ] && command -v wpa_cli >/dev/null 2>&1; then
        # Extract network blocks and configure wpa_supplicant
        SSID=$(grep 'ssid=' "$SAVED_WPA" | head -1 | sed 's/.*ssid="\(.*\)"/\1/')
        PSK=$(grep 'psk=' "$SAVED_WPA" | head -1 | sed 's/.*psk="\(.*\)"/\1/')
        if [ -n "$SSID" ] && [ -n "$PSK" ]; then
            echo "WiFi: restoring saved network '$SSID'"
            NETID=$(wpa_cli -i wlan0 add_network 2>/dev/null | tail -1)
            if [ -n "$NETID" ] && [ "$NETID" != "FAIL" ]; then
                wpa_cli -i wlan0 set_network "$NETID" ssid "\"$SSID\"" >/dev/null 2>&1
                wpa_cli -i wlan0 set_network "$NETID" psk "\"$PSK\"" >/dev/null 2>&1
                wpa_cli -i wlan0 enable_network "$NETID" >/dev/null 2>&1
                wpa_cli -i wlan0 select_network "$NETID" >/dev/null 2>&1
                echo "WiFi: network '$SSID' configured (id=$NETID)"
            fi
        fi
    fi

    return 0
}

platform_post_stop() {
    # Kill the keepalive process if still running
    if [ -n "$DRM_KEEPALIVE_PID" ]; then
        kill "$DRM_KEEPALIVE_PID" 2>/dev/null || true
        wait "$DRM_KEEPALIVE_PID" 2>/dev/null || true
        echo "DRM keepalive: cleaned up process $DRM_KEEPALIVE_PID"
        DRM_KEEPALIVE_PID=""
    fi

    # Do NOT restart /usr/bin/gui — the stock Snapmaker UI takes ownership of
    # wpa_supplicant on launch and drops the active WiFi connection, breaking
    # SSH and any in-flight install/update mid-stream (issue #797). Leaving the
    # display blank during stop is preferable to wedging the network. The stock
    # UI is restored by the uninstaller when the user explicitly removes us.
    return 0
}
