# AOWIS-SERVER-MAP
RESTful Caching Map Tile Server for [AOWIS-SERVER-GUI](https://github.com/aowis-org/AOWIS-SERVER-GUI).

**IMPORTANT:**

This server uses QHttpServer, which is **not suitable for exposure to the public Internet**. It does not provide HTTPS by itself.

By default, the server listens only on `127.0.0.1:8122`. Keep the port closed by the firewall when changing the listen address. If the GUI and map server run on different machines, use a properly secured VPN or a TLS reverse proxy.

## Build
### Linux
Run the script `compile_linux.sh`.

The result is created in `build-linux`.

### Windows / macOS
Windows and macOS are not officially supported at this point. The project can nevertheless be built like a regular Qt application.

## Configuration

Settings are resolved in this order:

1. Built-in defaults
2. INI configuration file
3. Explicit command-line options

When `--config` is omitted on Linux, the server checks the user-mode configuration file:

```text
~/.local/share/aowis-server-map/map-server.ini
```

If that file does not exist, the server continues with built-in defaults. It does not automatically probe `/etc`, because a manual user launch should not unexpectedly inherit system-wide credentials or server paths.

The supplied systemd unit explicitly selects the system configuration:

```text
/etc/aowis-server-map/map-server.ini
```

An explicitly requested file must exist and be valid:

```bash
./aowis-server-map --config /etc/aowis-server-map/map-server.ini
```

Create a documented default configuration without starting the server:

```bash
./aowis-server-map --write-default-config /etc/aowis-server-map/map-server.ini
```

The command refuses to replace an existing file. Replacement must be requested explicitly:

```bash
./aowis-server-map --write-default-config /etc/aowis-server-map/map-server.ini --overwrite
```

Generated configuration files are restricted to owner read/write permissions because they may later contain an API key.

Default INI content:

```ini
[server]
listen_address=127.0.0.1
port=8122
max_pending_requests=2048

[downloads]
max_active_downloads=32
cache_directory=

[authentication]
api_key=
```

## Server options

```text
-c, --config <path>                  Load settings from this INI file
--write-default-config <path>       Write built-in defaults and exit
--overwrite                         Allow default-config generation to replace an existing file
-a, --listen-address <address>      Override the listen IP address. Default: 127.0.0.1
-p, --port <port>                   Override the listen port. Default: 8122
--max-pending-requests <count>      Override maximum pending tile HTTP requests. Default: 2048
--max-active-downloads <count>      Override maximum simultaneous upstream downloads. Default: 32
--cache-directory <path>            Override the persistent tile-cache directory
--api-key <key>                     Override the API key for all non-OPTIONS endpoints
```

Example for loading a configuration file while overriding its port:

```bash
./aowis-server-map --config /etc/aowis-server-map/map-server.ini --port 9000
```

Example for listening on all IPv4 interfaces without a configuration file:

```bash
./aowis-server-map --listen-address 0.0.0.0 --port 8122
```

When an API key is configured, clients must send the key using either header:

```http
X-API-Key: your-secret-key
```

or:

```http
Authorization: Bearer your-secret-key
```

An empty `authentication/api_key` value disables authentication.

## User mode and systemd mode

A normal user launch without `--config` uses:

```text
Configuration: ~/.local/share/aowis-server-map/map-server.ini
Tile cache:    ~/.local/share/aowis-server-map/maptiles/
```

The service unit in `tools/systemd/aowis-server-map.service` uses:

```text
Configuration: /etc/aowis-server-map/map-server.ini
Tile cache:    /var/cache/aowis-server-map/maptiles/
```

The service does not require the application to guess that it is running under systemd. The unit explicitly passes the `/etc` configuration file and uses systemd's `CacheDirectory=aowis-server-map`. systemd creates `/var/cache/aowis-server-map` with the service account as owner and exports that path through `CACHE_DIRECTORY`. An explicit `downloads/cache_directory` setting or `--cache-directory` still overrides the automatic cache location.

Installation instructions are in `tools/systemd/README.md`.
