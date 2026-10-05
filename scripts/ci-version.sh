#!/bin/bash
# Prints the version used for workflow builds: the major.minor of
# app/version.txt followed by the UTC build time, e.g. 6.2.202610051530.
# Windows PE/MSI metadata can't hold that build field; see vrr-pe-version.ps1.
set -euo pipefail

SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE="$(tr -d '[:space:]' < "$SOURCE_ROOT/app/version.txt")"
if [[ ! "$BASE" =~ ^([0-9]+)\.([0-9]+)(\.[0-9]+)?$ ]]; then
    echo "app/version.txt has unexpected contents: '$BASE'" >&2
    exit 1
fi

echo "${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.$(date -u +%Y%m%d%H%M)"
