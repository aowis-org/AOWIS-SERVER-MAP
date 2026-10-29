#!/usr/bin/env bash
set -euo pipefail

: "${BASE_REVISION:?BASE_REVISION must be set}"
: "${UPGRADE_REVISION:?UPGRADE_REVISION must be set}"

export DEBIAN_FRONTEND=noninteractive

# A normal Docker container does not run systemd as PID 1. Do not try to start
# packaged daemons during apt transactions; runtime HTTP is tested explicitly.
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 0755 /usr/sbin/policy-rc.d

apt-get update -o APT::Update::Error-Mode=any
apt-get install --yes --no-install-recommends curl python3 util-linux

shopt -s nullglob
first_packages=(/packages/aowis-server-map_*"+deb${BASE_REVISION}"_*.deb)
upgrade_packages=(/packages/aowis-server-map_*"+deb${UPGRADE_REVISION}"_*.deb)
shopt -u nullglob
if (( ${#first_packages[@]} != 1 || ${#upgrade_packages[@]} != 1 )); then
    printf 'Error: expected exactly one base and upgrade package (%d, %d).\n' \
        "${#first_packages[@]}" "${#upgrade_packages[@]}" >&2
    exit 1
fi
first_package="${first_packages[0]}"
upgrade_package="${upgrade_packages[0]}"

printf '\n== Fresh Debian package installation ==\n'
apt-get install --yes --no-install-recommends "$first_package"
dpkg-query -W -f='${Status}\n' aowis-server-map | grep -Fxq 'install ok installed'

config=/etc/aowis-server-map/map-server.ini
unit=/usr/lib/systemd/system/aowis-server-map.service
binary=/usr/bin/aowis-server-map
state=/var/lib/aowis-server-map
cache=/var/cache/aowis-server-map

test -x "$binary"
test -f "$unit"
test -f "$config"
test -f /etc/apache2/sites-available/aowis-map.localhost.conf
test -f /etc/apache2/sites-available/aowis-server-map.conf
getent passwd aowis-server-map >/dev/null
getent group aowis-server-map >/dev/null

# Both keys are nonempty, independently generated and saved in the admin INI.
python3 - <<'PY'
import configparser
import re
settings = configparser.ConfigParser(interpolation=None)
assert settings.read('/etc/aowis-server-map/map-server.ini')
read_key = settings.get('authentication', 'api_key')
delete_key = settings.get('authentication', 'delete_api_key')
assert re.fullmatch('[0-9a-f]{64}', read_key)
assert re.fullmatch('[0-9a-f]{64}', delete_key)
assert read_key != delete_key
PY

"$binary" --check-config --config "$config" >/dev/null
"$binary" --print-default-config >/dev/null
systemd-analyze verify "$unit"

# systemd StateDirectory=/CacheDirectory= normally provisions these on start.
# Recreate that contract here, without changing the host or requiring systemd PID1.
install -d -o aowis-server-map -g aowis-server-map -m 0750 "$state" "$cache"

server_pid=''
cleanup_server()
{
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=''
    fi
}
trap cleanup_server EXIT

printf '\n== Installed binary HTTP smoke test ==\n'
read_key="$(sed -n 's/^api_key=//p' "$config" | head -n 1)"
setpriv --reuid "$(id -u aowis-server-map)" --regid "$(id -g aowis-server-map)" --init-groups \
    env STATE_DIRECTORY="$state" CACHE_DIRECTORY="$cache" \
    "$binary" --config "$config" --require-api-key --require-delete-api-key \
    --listen-address 127.0.0.1 --port 18122 \
    >/tmp/aowis-server-map-package-test.log 2>&1 &
server_pid=$!

ready=false
for attempt in $(seq 1 50); do
    if curl --silent --show-error --max-time 2 \
        -H "X-API-Key: $read_key" \
        -o /dev/null -f 'http://127.0.0.1:18122/status' 2>/dev/null; then
        ready=true
        break
    fi
    sleep 0.1
done
if [[ "$ready" != true ]]; then
    cat /tmp/aowis-server-map-package-test.log >&2
    echo 'Error: installed server did not serve authenticated /status.' >&2
    exit 1
fi

for path in /status /map/v1/providers /terrain/v1/datasets; do
    curl --fail --silent --show-error --max-time 5 \
        -H "X-API-Key: $read_key" \
        "http://127.0.0.1:18122$path" >/dev/null
done
unauthorized="$(curl --silent --max-time 5 -o /dev/null -w '%{http_code}' \
    'http://127.0.0.1:18122/status')"
if [[ "$unauthorized" != 401 ]]; then
    printf 'Error: unauthenticated /status returned HTTP %s instead of 401.\n' "$unauthorized" >&2
    exit 1
fi
cleanup_server
printf '%s\n' 'PASS: installed binary responds correctly with read-key authorization.'

printf '\n== Configuration and state preservation on upgrade ==\n'
cat >> "$config" <<'EOF_ADMIN'

; Administrator-owned configuration fixture: preserve this during upgrades.
EOF_ADMIN
chmod 0600 "$config"
config_hash="$(sha256sum "$config" | cut -d ' ' -f 1)"
config_mode="$(stat -c '%a:%U:%G' "$config")"
printf '[map_providers]\nconfigured=true\n' > "$state/providers.ini"
printf '%s\n' 'cache is preserved' > "$cache/package-smoke-cache"

apt-get install --yes --no-install-recommends "$upgrade_package"
[[ "$(sha256sum "$config" | cut -d ' ' -f 1)" == "$config_hash" ]]
[[ "$(stat -c '%a:%U:%G' "$config")" == "$config_mode" ]]
grep -Fxq 'configured=true' "$state/providers.ini"
grep -Fxq 'cache is preserved' "$cache/package-smoke-cache"
"$binary" --check-config --config "$config" >/dev/null
printf '%s\n' 'PASS: upgrade preserves administrator configuration, permissions and state.'

printf '\n== Package removal and reinstall ==\n'
apt-get remove --yes aowis-server-map
test -f "$config"
test -f "$state/providers.ini"
apt-get install --yes --no-install-recommends "$upgrade_package"
[[ "$(sha256sum "$config" | cut -d ' ' -f 1)" == "$config_hash" ]]
[[ "$(stat -c '%a:%U:%G' "$config")" == "$config_mode" ]]
test -f "$state/providers.ini"

printf '\n== Package purge ==\n'
apt-get purge --yes aowis-server-map
test ! -e "$config"
test -f "$state/providers.ini"
test -f "$cache/package-smoke-cache"
printf '%s\n' 'PASS: remove/reinstall preserves data; purge removes package configuration.'

printf '\nDebian package installation and lifecycle qualification PASSED.\n'
