#!/usr/bin/env bash
# Build-host only. Native raster processing; shell handles acquisition, locking,
# digests and Debian packaging. Never download at runtime.
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_name=ETOPO_2022_v1_60s_N90W180_surface.tif
url="https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/60s_surface_elev_gtif/$source_name"
pack_name=global-overview-v1.aowpack
manifest_name=etopo-2022-provenance.json
version=2022.1+overview1
deb_name="aowis-server-map-data_${version}_all.deb"
cache="${AOWIS_MAP_DATA_BUILD_CACHE:-${XDG_DATA_HOME:-$HOME/.local/share}/aowis-server-map/datasets/etopo-2022-v1-60s}"
local_source="${AOWIS_MAP_ETOPO_SOURCE:-$project_root/$source_name}"
native="$project_root/build-linux/aowis-map-etopo"
link_directory= deb_output= offline=false
fail() { printf 'ETOPO provisioning failed: %s\n' "$*" >&2; exit 1; }
while (( $# )); do
    case "$1" in
        --cache|--source|--native|--link-directory|--deb-output)
            (( $# >= 2 )) || fail "$1 requires a value"
            case "$1" in
                --cache) cache="$2";;
                --source) local_source="$2"; [[ -f "$2" ]] || fail "source file not found: $2";;
                --native) native="$2";;
                --link-directory) link_directory="$2";;
                --deb-output) deb_output="$2";;
            esac
            shift 2;;
        --offline) offline=true; shift;;
        --help|-h) echo "Usage: $0 [--native executable] [--cache directory] [--source TIFF] [--offline] [--link-directory dir] [--deb-output dir]"; exit 0;;
        *) fail "unknown option: $1";;
    esac
done
[[ -x "$native" ]] || fail "native preparation utility missing: $native (build it first)"
# Check tools and executable before touching the source or network.
for tool in flock sha256sum sed mktemp realpath; do
    command -v "$tool" >/dev/null || fail "required tool missing: $tool"
done
if [[ -n "$deb_output" ]]; then command -v dpkg-deb >/dev/null || fail 'dpkg-deb missing'; fi
"$native" self-test || fail 'native utility self-test failed'
mkdir -p "$cache"
cache="$(cd "$cache" && pwd -P)"
exec 9>"$cache/.prepare.lock"
flock -x 9
source="$cache/$source_name"; pack="$cache/$pack_name"; manifest="$cache/$manifest_name"
partial="$source.part"
sha() { local line; line="$(sha256sum < "$1")"; printf '%s\n' "${line%% *}"; }
# Read only the simple fixed string fields we write, also accepting the prior
# Python-generated manifest (same format/version). Reject missing/duplicate keys.
field() {
    local value
    value="$(sed -nE 's/^[[:space:]]*"'"$2"'"[[:space:]]*:[[:space:]]*"([^"]*)"[[:space:]]*,?[[:space:]]*$/\1/p' "$1")"
    [[ -n "$value" && "$value" != *$'\n'* ]] || return 1
    printf '%s\n' "$value"
}
old_source_hash= old_pack_hash=
if [[ -f "$manifest" ]]; then
    [[ "$(field "$manifest" version)" == "$version" ]] || fail 'unexpected data manifest version'
    old_source_hash="$(field "$manifest" source_sha256)" || fail 'missing source digest'
    old_pack_hash="$(field "$manifest" overview_sha256)" || fail 'missing overview digest'
    [[ "$old_source_hash" =~ ^[0-9a-f]{64}$ && "$old_pack_hash" =~ ^[0-9a-f]{64}$ ]] || fail 'invalid manifest digests'
fi
validated_source=false
if [[ -f "$source" ]]; then
    echo "Reusing original ETOPO: $source"
elif [[ -f "$local_source" ]]; then
    "$native" validate-source "$local_source" || fail 'local source validation failed'
    validated_source=true
    if [[ -n "$old_source_hash" && "$(sha "$local_source")" != "$old_source_hash" ]]; then fail 'local source differs from manifest'; fi
    temporary="$(mktemp "$cache/.source-import.XXXXXXXX")"
    if ! cp -- "$local_source" "$temporary" || ! mv -T -- "$temporary" "$source"; then
        rm -f -- "$temporary"; fail 'source import failed'
    fi
elif [[ -f "$partial" ]] && "$native" validate-source "$partial"; then
    validated_source=true
    echo "Reusing completed ETOPO download: $partial"
    if [[ -n "$old_source_hash" && "$(sha "$partial")" != "$old_source_hash" ]]; then fail 'partial source differs from manifest'; fi
    mv -T -- "$partial" "$source"
else
    if [[ "$offline" == true ]]; then fail 'source missing or partial invalid; --offline forbids downloading'; fi
    command -v curl >/dev/null || fail 'curl required to acquire missing source'
    # Do not delete an incomplete transfer; resume it. A structurally invalid
    # completed file also stays intact if NOAA rejects the range request.
    curl --fail --location --retry 3 --connect-timeout 30 --continue-at - --output "$partial" "$url"
    "$native" validate-source "$partial" || fail 'downloaded source validation failed; retained partial file'
    validated_source=true
    if [[ -n "$old_source_hash" && "$(sha "$partial")" != "$old_source_hash" ]]; then fail 'downloaded source differs from manifest'; fi
    mv -T -- "$partial" "$source"
fi
source_hash="$(sha "$source")"
[[ -z "$old_source_hash" || "$source_hash" == "$old_source_hash" ]] || fail 'stored ETOPO source changed; refusing automatic replacement'
if [[ -n "$old_pack_hash" && -f "$pack" && "$(sha "$pack")" == "$old_pack_hash" ]]; then
    pack_hash="$old_pack_hash"
    echo "Reusing global overview: $pack"
else
    if [[ "$validated_source" == false ]]; then
        "$native" validate-source "$source" || fail 'stored source validation failed'
    fi
    "$native" build "$source" "$pack" 1855 --clamp-negative
    pack_hash="$(sha "$pack")"
    temporary="$(mktemp "$cache/.manifest.XXXXXXXX")"
    cat > "$temporary" <<EOF_MANIFEST
{
  "version": "$version",
  "source": "$source_name",
  "source_url": "$url",
  "source_sha256": "$source_hash",
  "overview": "$pack_name",
  "overview_sha256": "$pack_hash",
  "license": "CC0-1.0",
  "attribution": "NOAA National Centers for Environmental Information, ETOPO 2022",
  "datum": "EGM2008",
  "resolution_arc_seconds": 60,
  "overview_processing": "Web Mercator z0-7, geographic pixel-footprint area averaging, negative elevations clamped to zero for visualization"
}
EOF_MANIFEST
    chmod 0644 "$temporary"
    mv -T -- "$temporary" "$manifest"
fi

build_deb() (
    local output="$cache/$deb_name" stamp="$cache/$deb_name.json" work temporary
    # Prior Python stamp also contains these simple fields inside provenance.
    if [[ -f "$output" && -f "$stamp" && "$(field "$stamp" version || true)" == "$version" \
          && "$(field "$stamp" source_sha256 || true)" == "$source_hash" \
          && "$(field "$stamp" overview_sha256 || true)" == "$pack_hash" \
          && "$(field "$stamp" sha256 || true)" == "$(sha "$output")" ]]; then
        echo "Reusing data package: $output"
        return
    fi
    work="$(mktemp -d "$cache/data-deb.XXXXXXXX")"
    trap 'rm -rf -- "$work"' EXIT
    mkdir -p "$work/root/DEBIAN" "$work/root/usr/share/aowis-server-map/terrain" "$work/root/usr/share/doc/aowis-server-map-data"
    cat > "$work/root/DEBIAN/control" <<EOF_CONTROL
Package: aowis-server-map-data
Version: $version
Architecture: all
Section: misc
Priority: optional
Maintainer: AOWIS Project <aowis@aowis.org>
Description: Permanent global ETOPO terrain data for AOWIS MAP
 Contains NOAA ETOPO 2022 60 arc-second source and visual terrain overview.
EOF_CONTROL
    install -m 0644 "$source" "$pack" "$manifest" "$work/root/usr/share/aowis-server-map/terrain/"
    install -m 0644 "$project_root/docs/etopo-copyright" "$work/root/usr/share/doc/aowis-server-map-data/copyright"
    dpkg-deb --root-owner-group -Zzstd --build "$work/root" "$work/$deb_name"
    mv -T -- "$work/$deb_name" "$output"
    temporary="$work/stamp.json"
    cat > "$temporary" <<EOF_STAMP
{
  "version": "$version",
  "source_sha256": "$source_hash",
  "overview_sha256": "$pack_hash",
  "sha256": "$(sha "$output")"
}
EOF_STAMP
    mv -T -- "$temporary" "$stamp"
)
if [[ -n "$deb_output" ]]; then
    build_deb
    mkdir -p "$deb_output"
    dest="$deb_output/$deb_name"
    if [[ "$(realpath -m "$dest")" != "$cache/$deb_name" ]] && { [[ ! -f "$dest" ]] || [[ "$(sha "$dest")" != "$(sha "$cache/$deb_name")" ]]; }; then
        temporary="$(mktemp "$deb_output/.data-deb.XXXXXXXX")"
        if ! cp -- "$cache/$deb_name" "$temporary" || ! chmod 0644 "$temporary" || ! mv -T -- "$temporary" "$dest"; then
            rm -f -- "$temporary"; fail 'data package export failed'
        fi
    fi
fi
if [[ -n "$link_directory" ]]; then
    mkdir -p "$link_directory"
    for name in "$source_name" "$pack_name" "$manifest_name"; do
        dest="$link_directory/$name"
        if [[ -L "$dest" && "$(realpath -m "$dest")" == "$cache/$name" ]]; then continue; fi
        [[ ! -e "$dest" && ! -L "$dest" ]] || fail "refusing to replace existing data: $dest"
        ln -s -- "$cache/$name" "$dest"
    done
fi
