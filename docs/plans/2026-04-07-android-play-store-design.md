# Android Google Play Store Publication — Design Spec

**Date:** 2026-04-07
**Status:** Approved
**Author:** Preston Brown + Claude

## Overview

Publish HelixScreen as an Open Beta (Early Access) on the Google Play Store. The Android build already exists and ships with nearly full feature parity. This effort focuses on fixing Android-specific issues, preparing store assets, setting up signing/build pipeline, and submitting.

## Goals

- **Primary use case:** Remote monitor/control of Klipper printers over local network
- **Future goal:** Direct-attached tablet replacement (full parity, Phase 2)
- **Release track:** Open Testing (Early Access)
- **Target audience:** Klipper enthusiasts (technical users)

## Constraints

- Local network only (no cloud, no auth beyond Moonraker)
- Manual IP entry (no mDNS discovery)
- Full UI ships as-is (not scoped down for mobile)
- Package name: `org.helixscreen.app` (already set, cannot change post-publish)

## Phase 1 — Code Fixes

### 1.1 Wizard Localhost Default (BLOCKER)

**File:** `src/ui/ui_wizard_connection.cpp`
**Problem:** Default IP is `127.0.0.1`, meaningless on Android.
**Fix:** Check `is_android_platform()` and default to empty string, forcing user to enter their printer's IP.

### 1.2 Android Back Button (BLOCKER)

**File:** Event handling in `src/application/application.cpp`
**Problem:** `SDL_SCANCODE_AC_BACK` not handled. Required for Play Store.
**Fix:** On back press, if NavigationManager has stack depth > 1, call `ui_nav_go_back()`. At root panel, do nothing (Android convention: don't quit on back from home).

### 1.3 Full Lifecycle Pause/Resume (BLOCKER)

**File:** `src/application/application.cpp`
**Problem:** No handling for `SDL_APP_WILLENTERBACKGROUND` / `SDL_APP_DIDENTERFOREGROUND`. WebSocket, rendering, polling all continue in background, draining battery.

**On entering background:**
- Close WebSocket connection entirely
- Stop LVGL tick/render loop
- Pause temperature/status polling timers
- Stop webcam/camera feed if streaming
- Mute sound sequencer

**On returning to foreground:**
- Reconnect WebSocket, re-subscribe to printer state
- Resume LVGL tick/render loop
- Force full state refresh (printer state may have changed)
- Resume camera feed if it was active
- Unmute sound
- Invalidate display (force full redraw, framebuffer may be stale)

### 1.4 Cache Directory Android Path

**File:** `src/app_globals.cpp`
**Problem:** Cache directory resolution falls through to `/tmp` on Android.
**Fix:** Add Android-first path using `SDL_AndroidGetInternalStoragePath()` before the Linux fallback chain.

### 1.5 Guard USB Printer Paths

**File:** `src/system/phomemo_printer.cpp` (and similar USB label printer backends)
**Problem:** Accesses `/dev/usb/lp*` and `/sys/class/usbmisc/` which don't exist on Android.
**Fix:** Guard with `!is_android_platform()` check or `#ifndef __ANDROID__`.

### 1.6 DPI Awareness

**File:** Display initialization code
**Problem:** Same pixel layout on 6" phone and 13" tablet.
**Fix:** Query `SDL_GetDisplayDPI()` at startup and use it to inform LVGL display resolution config. Ensure UI is usable across Android device sizes.

### 1.7 Manifest Cleanup

**File:** `android/app/src/main/AndroidManifest.xml`
**Fix:** Verify existing permissions are sufficient. No VIBRATE needed (no haptics). No BLUETOOTH needed yet (plugin excluded from Android build). Current permissions (INTERNET, ACCESS_NETWORK_STATE) should be sufficient for Phase 1.

## Phase 2 — Store Assets

### 2.1 Screenshots

- Generate 8 screenshots from mock mode (`--test`) using existing `scripts/screenshot.sh`
- Key panels: Dashboard, Print Status, Temperature Graph, Bed Mesh 3D, Motion Controls, File Browser, Filament/AMS, Settings/Themes
- Phone format (16:9 landscape) required, tablet (16:10) recommended

### 2.2 Feature Graphic

- 1024x500 banner for Play Store listing header
- Crop existing splash screen logo/logotype to this aspect ratio
- Source assets: built-in splash screen images

### 2.3 Store Listing Text

**Short description (80 chars):**
> Klipper touchscreen UI — monitor and control your 3D printer from any Android device.

**Full description:**
> HelixScreen is a full-featured touch interface for Klipper 3D printers. Connect to your printer over your local network and get the same powerful UI that runs on dedicated touchscreens — right on your phone or tablet.
>
> **Features:**
> - 30+ panels: dashboard, temperature graphs, motion control, bed mesh 3D view, console, macros, and more
> - Multi-material support for AFC, Happy Hare, ACE, AD5X IFS, CFS, Snapmaker U1, and tool changers
> - Exclude objects mid-print — tap the failing part on an overhead map
> - 80+ printer models auto-detected with first-run wizard
> - Calibration suite: input shaper, PID tuning, Z-offset, firmware retraction
> - 17 themes with light/dark mode
> - 9 languages
>
> **Requirements:** A 3D printer running Klipper + Moonraker on the same local network.
>
> **Open source (GPL v3)** — no accounts, no cloud, no tracking.

**Category:** Tools
**Content rating:** Everyone

### 2.4 Privacy Policy

- Existing policy at `docs/user/PRIVACY_POLICY.md`
- Publish to helixscreen.org and use that URL in Play Store listing
- Contact: privacy@helixscreen.org
- Entity: 356C LLC

## Phase 3 — Build & Signing

### 3.1 Upload Keystore

- Generate upload keystore for signing AABs
- Google Play App Signing manages the distribution key
- Upload key is replaceable if lost (unlike legacy model)
- Store keystore securely outside the repository

### 3.2 Gradle AAB Configuration

- CI currently runs `assembleRelease` producing per-arch APKs (arm64, x86_64, universal)
- Add `bundleRelease` step alongside existing APK build (keep APKs for direct download, add AAB for Play Store)
- Play Store requires AAB format
- Verify the signed AAB installs and runs correctly on a test device

### 3.3 CI Integration

- Update `.github/workflows/release.yml` to also produce AAB artifact
- Upload keystore secrets stored in GitHub Actions secrets
- AAB uploaded as release artifact alongside existing APKs

## Phase 4 — Play Store Submission

### 4.1 Developer Account Setup (Manual)

- Register at play.google.com/console
- $25 one-time fee
- Identity verification as 356C LLC
- Estimated: 1-2 business days for org verification

### 4.2 Create App Listing (Manual)

- Create new app in Play Console
- Upload all assets (screenshots, feature graphic, icon)
- Fill in listing text, category, content rating, privacy policy URL
- Contact email for listing

### 4.3 Upload & Submit

- Upload AAB to Open Testing track
- Mark as Early Access
- Submit for review (open testing has minimal review delay)

## Phase 1 Addendum — Update Flow on Android

### Update Detection → Play Store Redirect

The existing update checker detects new releases via GitHub/Moonraker. On non-Android platforms, it offers in-app download and install. On Android:

- **Keep:** Update detection, alert dialog, "Update Available" badge in Settings → About
- **Change:** Instead of initiating an in-app update, the "Update" action opens the Play Store listing for HelixScreen using an Android intent:
  ```
  market://details?id=org.helixscreen.app
  ```
  Falls back to the Play Store web URL if the Play Store app isn't available:
  ```
  https://play.google.com/store/apps/details?id=org.helixscreen.app
  ```
- **Implementation:** Add `is_android_platform()` check in the update action handler. Use SDL's `SDL_OpenURL()` to launch the intent (SDL2 handles the `market://` scheme on Android via JNI).
- **Remove on Android:** Download progress bar, install/apply logic, restart prompt

## What's NOT in Scope

- Cloud relay / remote access beyond local network
- mDNS printer discovery
- Android Bluetooth backend (future Phase 2 work)
- Android WiFi/Ethernet management backends
- Push notifications
- Portrait orientation support
