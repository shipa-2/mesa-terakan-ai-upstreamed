#!/bin/bash
# Apply all Terakan mimo-development patches to a Mesa tree.
set -euo pipefail

MESA_DIR="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_DIR="$(cd "$SCRIPT_DIR/../patches" && pwd)"

if [[ -z "$MESA_DIR" || ! -d "$MESA_DIR/.git" ]]; then
  echo "Usage: $0 /path/to/mesa-checkout"
  echo "Clone upstream first:"
  echo "  git clone --branch Terakan_state_rework --single-branch \\"
  echo "    https://gitlab.freedesktop.org/Triang3l/mesa.git mesa"
  exit 1
fi

apply_one() {
  local patch="$1"
  echo "Applying $(basename "$patch")..."
  git -C "$MESA_DIR" apply "$patch"
}

for patch in \
  "$PATCH_DIR"/0001-fix-c23.patch \
  "$PATCH_DIR"/0002-fix-c23-pthread-casts.patch \
  "$PATCH_DIR"/0003-bump-api-version-1.1.patch \
  "$PATCH_DIR"/0004-implement-cmd-blit-image2.patch \
  "$PATCH_DIR"/0010-compute-mvp.patch; do
  apply_one "$patch"
done

echo "All patches applied to $MESA_DIR"
