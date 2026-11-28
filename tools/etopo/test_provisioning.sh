#!/usr/bin/env bash
# Exercise shell orchestration without network or a large data dependency.
set -euo pipefail
prepare="$1"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
export TEST_WORK="$work"
mkdir -p "$work/bin" "$work/cache" "$work/incomplete"
cat > "$work/bin/curl" <<'MOCK'
#!/usr/bin/env bash
touch "$TEST_WORK/network-called"
exit 99
MOCK
cat > "$work/bin/native" <<'MOCK'
#!/usr/bin/env bash
set -e
case "$1" in
    self-test) ;;
    validate-source) [[ "$(cat "$2")" == 'complete original' ]];;
    build) printf 'overview fixture' > "$3"; echo built >> "$TEST_WORK/builds";;
    *) exit 1;;
esac
MOCK
chmod +x "$work/bin/"{curl,native}
export PATH="$work/bin:$PATH"
source_name=ETOPO_2022_v1_60s_N90W180_surface.tif
source="$work/cache/$source_name"
pack="$work/cache/global-overview-v1.aowpack"
run() { bash "$prepare" --native "$work/bin/native" --cache "$work/cache" "$@"; }
printf 'complete original' > "$source.part"
run --offline --link-directory "$work/links"
[[ -f "$source" && ! -e "$source.part" && -f "$pack" && -L "$work/links/$source_name" ]]
run --offline
[[ "$(wc -l < "$work/builds")" == 1 ]]
rm "$pack"
run --offline
[[ "$(wc -l < "$work/builds")" == 2 ]]
# Corrupted originals must not be replaced or reacquired.
printf 'changed original' > "$source"
if run --offline; then exit 1; fi
[[ "$(cat "$source")" == 'changed original' ]]
printf 'incomplete' > "$work/incomplete/$source_name.part"
if bash "$prepare" --native "$work/bin/native" --cache "$work/incomplete" --offline; then exit 1; fi
[[ "$(cat "$work/incomplete/$source_name.part")" == incomplete ]]
[[ ! -e "$work/network-called" ]]
