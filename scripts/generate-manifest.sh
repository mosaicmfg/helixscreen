#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# generate-manifest.sh — Generate manifest.json from a directory of release tarballs.
# Shared by CI (release.yml) and local dev releases (dev-release.sh).

set -euo pipefail

# Default values
VERSION=""
TAG=""
NOTES=""
DIR=""
BASE_URL=""
OUTPUT=""
INCLUDE_ZIP=false

usage() {
    cat <<EOF
Usage: generate-manifest.sh --version VERSION --tag TAG --notes NOTES --dir DIR --base-url URL --output FILE [--include-zip]

Generate a manifest.json from release tarballs in DIR.

Options:
  --version VERSION   Version string (e.g., "0.9.5")
  --tag TAG           Git tag (e.g., "v0.9.5")
  --notes NOTES       Release notes text
  --dir DIR           Directory containing helixscreen-{platform}-*.tar.gz files
  --base-url URL      Base URL for download links (e.g., "https://releases.helixscreen.org/dev")
  --output FILE       Output manifest.json path
  --include-zip       Include zip_url/zip_sha256 fields when a .zip is present.
                      Off by default: pre-v0.99.31 in-app updaters call gunzip
                      on whatever URL the manifest hands them, so a .zip in the
                      manifest causes 'Corrupt download' for users still on
                      v0.99.30 or earlier (#797 follow-up). Re-enable once
                      adoption telemetry shows v0.99.30 is gone.
  --help              Show this help message
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --version)     VERSION="$2";     shift 2 ;;
        --tag)         TAG="$2";         shift 2 ;;
        --notes)       NOTES="$2";       shift 2 ;;
        --dir)         DIR="$2";         shift 2 ;;
        --base-url)    BASE_URL="$2";    shift 2 ;;
        --output)      OUTPUT="$2";      shift 2 ;;
        --include-zip) INCLUDE_ZIP=true; shift ;;
        --help)        usage ;;
        *)
            echo "Error: Unknown option $1" >&2
            exit 1
            ;;
    esac
done

# Validate required arguments
missing=()
[[ -z "$VERSION" ]] && missing+=("--version")
[[ -z "$TAG" ]]     && missing+=("--tag")
[[ -z "$DIR" ]]     && missing+=("--dir")
[[ -z "$BASE_URL" ]] && missing+=("--base-url")
[[ -z "$OUTPUT" ]]  && missing+=("--output")

if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Error: Missing required arguments: ${missing[*]}" >&2
    exit 1
fi

if [[ ! -d "$DIR" ]]; then
    echo "Error: Directory not found: $DIR" >&2
    exit 1
fi

# Check required tools
if ! command -v jq &>/dev/null; then
    echo "Error: jq not found. Please install it." >&2
    exit 1
fi

# Determine sha256 command
if command -v shasum &>/dev/null; then
    SHA256_CMD="shasum -a 256"
elif command -v sha256sum &>/dev/null; then
    SHA256_CMD="sha256sum"
else
    echo "Error: Neither shasum nor sha256sum found" >&2
    exit 1
fi

# Auto-discover platforms from tarballs in DIR. Filename convention is
# `helixscreen-{platform}-{version}.tar.gz` where {version} starts with 'v'
# or a digit (e.g., v0.99.31, 0.99.31). Auto-discovery keeps this script in
# sync with whatever .github/workflows/release.yml uploads — adding a new
# platform to the release matrix doesn't require editing this file.
FOUND_ANY=false
ASSETS_JSON="{}"
PLATFORMS=()
for f in "$DIR"/helixscreen-*-*.tar.gz; do
    [[ -f "$f" ]] || continue
    base=$(basename "$f")
    # Strip leading 'helixscreen-' and trailing '-{version}.tar.gz' to recover
    # the platform key. Version segment must start with 'v' or a digit so that
    # platforms whose names contain hyphens (e.g. snapmaker-u1) survive.
    if [[ "$base" =~ ^helixscreen-(.+)-v?[0-9][^-]*\.tar\.gz$ ]]; then
        PLATFORMS+=("${BASH_REMATCH[1]}")
    fi
done

for plat in "${PLATFORMS[@]}"; do
    tarball=""
    for f in "$DIR"/helixscreen-"${plat}"-*.tar.gz; do
        if [[ -f "$f" ]]; then
            tarball="$f"
            break
        fi
    done

    if [[ -z "$tarball" ]]; then
        continue
    fi

    FOUND_ANY=true
    filename=$(basename "$tarball")
    sha256=$($SHA256_CMD "$tarball" | awk '{print $1}')
    # `wc -c` is portable across Linux/macOS/BSD (stat(1) flags differ:
    # `-c '%s'` GNU vs `-f '%z'` BSD). The in-app updater reads `size` to
    # compute the staging-directory free-space requirement (1.2× + small
    # buffer); omitting it forces a conservative fixed-size fallback.
    size=$(wc -c < "$tarball" | tr -d ' ')
    url="${BASE_URL}/${filename}"

    ASSETS_JSON=$(echo "$ASSETS_JSON" | jq \
        --arg plat "$plat" \
        --arg url "$url" \
        --arg sha256 "$sha256" \
        --argjson size "$size" \
        '.[$plat] = {url: $url, sha256: $sha256, size: $size}')

    # Check for corresponding ZIP file (used by Moonraker type:zip updates).
    # Gated behind --include-zip until pre-v0.99.31 in-app updaters age out.
    if [[ "$INCLUDE_ZIP" == true ]]; then
        zipfile="$DIR/helixscreen-${plat}.zip"
        if [[ -f "$zipfile" ]]; then
            zip_sha256=$($SHA256_CMD "$zipfile" | awk '{print $1}')
            zip_size=$(wc -c < "$zipfile" | tr -d ' ')
            zip_url="${BASE_URL}/helixscreen-${plat}.zip"

            ASSETS_JSON=$(echo "$ASSETS_JSON" | jq \
                --arg plat "$plat" \
                --arg zip_url "$zip_url" \
                --arg zip_sha256 "$zip_sha256" \
                --argjson zip_size "$zip_size" \
                '.[$plat] += {zip_url: $zip_url, zip_sha256: $zip_sha256, zip_size: $zip_size}')
        fi
    fi
done

if [[ "$FOUND_ANY" == "false" ]]; then
    echo "Error: No helixscreen-*.tar.gz tarballs found in $DIR" >&2
    exit 1
fi

# Generate timestamp
PUBLISHED_AT=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

# Build final manifest
jq -n \
    --arg version "$VERSION" \
    --arg tag "$TAG" \
    --arg notes "${NOTES:-}" \
    --arg published_at "$PUBLISHED_AT" \
    --argjson assets "$ASSETS_JSON" \
    '{
        version: $version,
        tag: $tag,
        notes: $notes,
        published_at: $published_at,
        assets: $assets
    }' > "$OUTPUT"

echo "Generated $OUTPUT with platforms: $(echo "$ASSETS_JSON" | jq -r 'keys | join(", ")')"
