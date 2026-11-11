#!/usr/bin/env bash
set -euo pipefail

# A ZIP patch cannot contain a Git submodule gitlink (mode 160000). This
# bootstraps it once; subsequent clones use the pinned commit from Git.
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd -- "$project_root"

if [[ "$(git rev-parse --show-toplevel 2>/dev/null)" != "$project_root" ]]; then
    printf 'Error: run from an AOWIS-SERVER-MAP Git checkout.\n' >&2
    exit 1
fi

path=external/AOWIS-CONFIG
url=https://github.com/aowis-org/AOWIS-CONFIG.git

if git ls-files --stage -- "$path" | grep -q '^160000 '; then
    # Do not move an existing pinned submodule to the tip of main.
    git submodule sync -- "$path"
    git submodule update --init --checkout -- "$path"
else
    if [[ -e "$path" || -L "$path" ]]; then
        printf 'Error: %s exists but is not a registered Git submodule.\n' "$path" >&2
        exit 1
    fi
    git submodule add -- "$url" "$path"
    # git submodule add stages .gitmodules and the 160000 gitlink. Commit both.
fi

printf 'AOWIS-CONFIG pinned at %s\n' "$(git -C "$path" rev-parse HEAD)"
