#!/usr/bin/env bash
set -euo pipefail

service_name="aowis-server-map"
service_user="aowis-server-map"
service_group="aowis-server-map"
install_binary="/usr/bin/aowis-server-map"
config_directory="/etc/aowis-server-map"
config_file="$config_directory/map-server.ini"
unit_file="/etc/systemd/system/$service_name.service"
script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_directory="$(cd -- "$script_directory/../.." && pwd)"
source_binary="$project_directory/build-linux/aowis-server-map"
enable_now=false
temporary_config=""

cleanup()
{
    if [[ -n "$temporary_config" && -e "$temporary_config" ]]; then
        rm -f -- "$temporary_config"
    fi
}
trap cleanup EXIT

print_usage()
{
    cat <<EOF_USAGE
Usage: sudo $0 [--binary <path>] [--enable-now]

Options:
  --binary <path>  Install this binary instead of the default build output:
                   $source_binary
  --enable-now     Enable and start the service after installation.
  -h, --help       Show this help.

On first installation, the installer creates $config_file with separate random
read and delete API keys and prints both keys. Existing configuration files are
never overwritten and existing keys are never changed.
EOF_USAGE
}

require_argument()
{
    if [[ $# -lt 2 || -z "$2" ]]; then
        echo "Error: $1 requires a value." >&2
        exit 2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)
            require_argument "$@"
            source_binary="$2"
            shift 2
            ;;
        --enable-now)
            enable_now=true
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "Error: unknown option: $1" >&2
            print_usage >&2
            exit 2
            ;;
    esac
done

if [[ $EUID -ne 0 ]]; then
    echo "Error: run this installer as root, for example with sudo." >&2
    exit 1
fi

for command_name in chmod getent groupadd install mktemp openssl rm systemctl useradd; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "Error: required command is unavailable: $command_name" >&2
        exit 1
    fi
done

if [[ ! -f "$source_binary" || ! -x "$source_binary" ]]; then
    echo "Error: map-server binary not found or not executable: $source_binary" >&2
    echo "Build it first or pass --binary <path>." >&2
    exit 1
fi

if ! getent group "$service_group" >/dev/null; then
    groupadd --system "$service_group"
    echo "Created system group: $service_group"
fi

if ! getent passwd "$service_user" >/dev/null; then
    useradd --system --gid "$service_group" --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin "$service_user"
    echo "Created system user: $service_user"
fi

install -m 0755 "$source_binary" "$install_binary"
install -d -o root -g "$service_group" -m 0750 "$config_directory"

created_config=false
api_key=""
delete_api_key=""
if [[ -e "$config_file" || -L "$config_file" ]]; then
    if [[ ! -f "$config_file" || ! -r "$config_file" ]]; then
        echo "Error: existing configuration path is not a readable file: $config_file" >&2
        exit 1
    fi
    echo "Preserved existing configuration: $config_file"
    echo "Verify that authentication/api_key and authentication/delete_api_key are both set."
else
    api_key="$(openssl rand -hex 32)"
    delete_api_key="$(openssl rand -hex 32)"
    while [[ "$delete_api_key" == "$api_key" ]]; do
        delete_api_key="$(openssl rand -hex 32)"
    done
    temporary_config="$(mktemp)"
    chmod 0600 "$temporary_config"

    while IFS= read -r line || [[ -n "$line" ]]; do
        case "$line" in
            api_key=)
                printf 'api_key=%s\n' "$api_key"
                ;;
            delete_api_key=)
                printf 'delete_api_key=%s\n' "$delete_api_key"
                ;;
            *)
                printf '%s\n' "$line"
                ;;
        esac
    done < "$script_directory/map-server.ini" > "$temporary_config"

    install -m 0640 -o root -g "$service_group" "$temporary_config" "$config_file"
    created_config=true
    echo "Installed configuration with generated API keys: $config_file"
fi

install -m 0644 "$script_directory/$service_name.service" "$unit_file"
systemctl daemon-reload

echo "Installed binary: $install_binary"
echo "Installed systemd unit: $unit_file"
echo "Edit the configuration with: sudoedit $config_file"

if [[ "$created_config" == true ]]; then
    cat <<EOF_KEYS

Generated API keys
------------------
Read API key:   $api_key
Delete API key: $delete_api_key

The keys are stored in $config_file. The read key authorizes status and tile GET
requests. The delete key authorizes cache DELETE requests. Record them now for
the clients or reverse proxy configuration that will use them.
EOF_KEYS
fi

if [[ "$enable_now" == true ]]; then
    systemctl enable --now "$service_name.service"
    echo "Enabled and started: $service_name.service"
else
    echo "Start the service with:"
    echo "  sudo systemctl enable --now $service_name.service"
fi
