#!/usr/bin/env bash
# Aggregate L081 Mechanism D (freeze-drop) events from device logs.
#
# The Mechanism D detector logs `[UpdateQueue] DROPPED (frozen): <tag>` every
# time a `tok.defer(...)` / `queue_update(...)` is silently discarded during a
# `scoped_freeze()` window. If <tag> identifies a first-fire baseline-state
# callback (HTTP response that the UI is waiting on), the drop leaves the UI
# permanently broken. This script aggregates drops across one or more log
# sources so we can spot new regressions without manually walking each device.
#
# Usage:
#   scripts/freeze-drops.sh <log-file> [<log-file> ...]
#   scripts/freeze-drops.sh --ssh <host> [--ssh <host> ...]
#   scripts/freeze-drops.sh --bundle <debug-bundle.json> [...]
#
# Examples:
#   # Aggregate across multiple devices via SSH
#   scripts/freeze-drops.sh --ssh root@192.168.1.74 --ssh root@192.168.30.103
#
#   # Aggregate from saved debug bundles
#   scripts/freeze-drops.sh --bundle /tmp/debug-bundle-*.json
#
#   # Local file
#   scripts/freeze-drops.sh /tmp/helixscreen.log
#
# Cross-references each tag to a source location and notes whether it's
# already using defer_critical (existing fix) or plain defer (regression).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

inputs=()
mode="files"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ssh)   mode="ssh";    inputs+=("$2"); shift 2 ;;
        --bundle) mode="bundle"; inputs+=("$2"); shift 2 ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) inputs+=("$1"); shift ;;
    esac
done

if [[ ${#inputs[@]} -eq 0 ]]; then
    echo "Usage: $0 <log-file> [...]  |  --ssh <host> [...]  |  --bundle <file.json> [...]" >&2
    exit 1
fi

agg="$TMPDIR/all.txt"
: > "$agg"

for src in "${inputs[@]}"; do
    case "$mode" in
        ssh)
            # Try /tmp/helixscreen.log first, fall back to /var/log/messages
            ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no "$src" \
                "grep 'DROPPED (frozen)' /tmp/helixscreen.log 2>/dev/null || \
                 grep 'DROPPED (frozen)' /var/log/messages 2>/dev/null || true" \
                2>/dev/null | sed "s|^|[$src] |" >> "$agg" || \
                echo "WARN: could not fetch from $src" >&2
            ;;
        bundle)
            # Bundles store logs under crash_txt or system.log
            if command -v jq >/dev/null 2>&1; then
                jq -r '.crash_txt // .system.log // empty' "$src" 2>/dev/null \
                    | grep 'DROPPED (frozen)' \
                    | sed "s|^|[$(basename "$src")] |" >> "$agg" || true
            else
                echo "WARN: jq not installed, skipping bundle $src" >&2
            fi
            ;;
        files)
            grep 'DROPPED (frozen)' "$src" 2>/dev/null \
                | sed "s|^|[$(basename "$src")] |" >> "$agg" || true
            ;;
    esac
done

total="$(wc -l < "$agg" | tr -d ' ')"
if [[ "$total" -eq 0 ]]; then
    echo "✓ No freeze-drop events found across ${#inputs[@]} source(s)."
    exit 0
fi

echo "═══ L081 Mechanism D freeze-drops ═══"
echo "Total events: $total across ${#inputs[@]} source(s)"
echo ""
echo "──── By tag (drops × tag) ────"
# Extract the tag (after "DROPPED (frozen): ") and aggregate
sed -E 's/.*DROPPED \(frozen\): //' "$agg" | sort | uniq -c | sort -rn

echo ""
echo "──── Source location + defer type ────"
# For each unique tag, find its definition in the source tree and note defer type
sed -E 's/.*DROPPED \(frozen\): //' "$agg" | sort -u | while read -r tag; do
    [[ -z "$tag" ]] && continue
    hit="$(cd "$REPO_ROOT" && grep -rn "\"$tag\"" src include 2>/dev/null \
        | grep -E 'defer|queue_' | head -1)"
    if [[ -z "$hit" ]]; then
        echo "  $tag → (no source match — stale tag?)"
        continue
    fi
    file_line="$(echo "$hit" | cut -d: -f1-2)"
    if echo "$hit" | grep -q 'defer_critical\|queue_critical'; then
        status="✓ defer_critical (drop is from old binary; should not recur)"
    else
        status="✗ plain defer — REGRESSION CANDIDATE, convert to defer_critical"
    fi
    echo "  $tag"
    echo "    $file_line"
    echo "    $status"
done

echo ""
echo "──── Per-source breakdown ────"
# First bracketed field is the source tag we prepended.
awk -F'[][]' '{print $2}' "$agg" | sort | uniq -c | sort -rn
