#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s /path/to/patch.zip\n' "${0##*/}" >&2
    exit 2
fi

# AOWIS-SERVER-MAP accepts its own patches only, named aowis-server-map-*.zip. Check
# the archive's basename before resolving or opening it, so a patch for another
# AOWIS project, or a renamed or misplaced one, is rejected without changing
# the worktree.
patch_name="${1##*/}"
case "$patch_name" in
    aowis-server-map-?*.zip) ;;
    *)
        printf 'Error: patch ZIP filename must start with aowis-server-map- and end with .zip: %s\n' "$patch_name" >&2
        exit 2
        ;;
esac

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_root="$(cd -- "$script_dir/.." && pwd -P)"
parent="$(dirname -- "$project_root")"
project_name="$(basename -- "$project_root")"
patch="$(realpath -- "$1")"

# Only accept archives containing paths inside this repository.
while IFS= read -r entry; do
    if [[ "$entry" != "$project_name/"* || "$entry" == *'/../'* ||
          "$entry" == *'/./'* || "$entry" == *'\\'* ]]; then
        printf 'Invalid patch path: %s\n' "$entry" >&2
        exit 1
    fi
done < <(unzip -Z -1 "$patch")

unzip -q -o "$patch" -d "$parent"

# ZIP timestamps reflect archive creation, not application. Refresh both
# sources and directories here so Ninja sees changes when applying a patch.
while IFS= read -r entry; do
    touch -c -m -- "$parent/$entry"
done < <(unzip -Z -1 "$patch")

printf 'Applied %s with current file and directory timestamps.\n' "$patch"
