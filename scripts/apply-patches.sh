#!/bin/bash
# Apply all Terakan AI development patches to a Mesa tree.
set -euo pipefail

MESA_DIR="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_DIR="$(cd "$SCRIPT_DIR/../patches" && pwd)"

if [[ -z "$MESA_DIR" || ! -d "$MESA_DIR/.git" ]]; then
  echo "Usage: $0 /path/to/mesa-checkout"
  echo "Clone upstream first:"
  echo "  git clone -b Terakan_Backup_2026-06-10_2_Meta_MSAA \\"
  echo "    https://gitlab.freedesktop.org/Triang3l/mesa.git mesa"
  exit 1
fi

for patch in "$PATCH_DIR"/000*.patch; do
  echo "Applying $(basename "$patch")..."
  patch -d "$MESA_DIR" -Np1 -i "$patch"
done

echo "All patches applied to $MESA_DIR"
