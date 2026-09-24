#!/bin/bash
# Obtain Microsoft's DirectX runtime DLLs for this local build.
#
# Microsoft ships no single-DLL download for DirectX, only the End-User
# Runtimes redistributable (the June 2010 one is the last, and is the same file
# winetricks' `directx9` verb uses). It is redistributable with an application,
# which is what makes it usable here at all. tools/extract-directx.py carves the
# cabinet out of the self-extracting archive and installs the modules listed in
# tools/directx-component.txt into app/Madeira/x86_64-directx/, checking each one
# is x64 on the way in.
#
# The result is a directory of unmodified Microsoft binaries. It is not committed
# (see app/Madeira/legal/THIRD-PARTY-NOTICES.md) and the IPA workflow runs this
# before packaging, so a release always has them.
#
#   DIRECTX_URL       override the download (a mirrored or authenticated copy)
#   DIRECTX_INSTALLER use an already-downloaded redistributable instead
#   DIRECTX_OUT       install directory (default app/Madeira/x86_64-directx)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${DIRECTX_OUT:-$REPO_ROOT/app/Madeira/x86_64-directx}"
URL="${DIRECTX_URL:-https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/directx_Jun2010_redist.exe}"

command -v python3 >/dev/null || { echo "Install Python 3 first." >&2; exit 1; }
if ! command -v cabextract >/dev/null && ! command -v 7zz >/dev/null && ! command -v 7z >/dev/null; then
    echo "Install a cabinet extractor first (Debian/Ubuntu: apt-get install cabextract;" >&2
    echo "macOS: brew install cabextract)." >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/madeira-directx.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
INSTALLER="${DIRECTX_INSTALLER:-}"
if [[ -z "$INSTALLER" ]]; then
    command -v curl >/dev/null || { echo "Install curl first." >&2; exit 1; }
    INSTALLER="$WORK/directx_redist.exe"
    echo "Downloading Microsoft DirectX End-User Runtimes (June 2010)..."
    # Do not echo the URL: CI may supply an authenticated one through a secret.
    curl --fail --silent --show-error --location --retry 3 \
        --connect-timeout 30 --max-time 1800 --proto '=https' --proto-redir '=https' \
        --output "$INSTALLER" "$URL"
fi

python3 "$REPO_ROOT/tools/extract-directx.py" "$INSTALLER" --out "$OUT"
