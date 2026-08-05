# AOWIS-SERVER-MAP
RESTful caching map tile server for [AOWIS-SERVER-GUI](https://github.com/aowis-org/AOWIS-SERVER-GUI).

**IMPORTANT:**

This server uses QHttpServer and must not be exposed directly to the public Internet. Put it behind a TLS reverse proxy and restrict direct access to the map-server port by firewall.

By default, the server listens only on `127.0.0.1:8122`. When the reverse proxy runs on another machine, bind the map server to a private-network address and permit port 8122 only from the proxy host. A second reverse proxy on the map-server machine is unnecessary.

## Build

### Linux

On Debian or a Debian-based distribution, install the required compiler, CMake, Qt 6 base, WebSockets and HttpServer development packages and build without starting the server:

```bash
./compile_linux.sh --install-dependencies --no-run
```

The script verifies `/etc/os-release` before using `apt-get`. On other Linux distributions, install the equivalent dependencies with the distribution's package manager and run:

```bash
./compile_linux.sh --no-run
```

To build and immediately start the server, omit `--no-run`:

```bash
./compile_linux.sh
```

Arguments not handled by the build script are passed to the server. Use `--` when a server argument has the same name as a build-script option.

The executable is created as:

```text
build-linux/aowis-server-map
```

### Windows / macOS

Windows and macOS are not officially supported at this point. The project can nevertheless be built like a regular Qt application.

## Configuration

Settings are resolved in this order:

1. Built-in defaults
2. INI configuration file
3. Explicit command-line options

When `--config` is omitted on Linux, the server checks:

```text
~/.local/share/aowis-server-map/map-server.ini
```

If the file does not exist, the server continues with built-in defaults. It does not probe `/etc`, because a manual user launch should not unexpectedly inherit system-wide credentials or server paths.

The supplied systemd unit explicitly uses:

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
delete_api_key=
```

`api_key` protects `/status` and tile GET requests. An empty value disables read authentication in normal user mode.

`delete_api_key` protects cache DELETE requests. An empty delete key disables cache deletion; it never makes deletion public.

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
--api-key <key>                     Override the API key for status and tile GET requests
--delete-api-key <key>              Override the separate cache DELETE API key
--require-api-key                   Refuse startup when no read API key is configured
--require-delete-api-key            Refuse startup when no delete API key is configured
```

Both endpoint-specific keys use either header:

```http
X-API-Key: endpoint-specific-key
```

or:

```http
Authorization: Bearer endpoint-specific-key
```

Use the normal key for `/status` and tiles. Use the delete key only for the cache DELETE endpoint.

## User mode and systemd mode

A normal user launch without `--config` uses:

```text
Configuration: ~/.local/share/aowis-server-map/map-server.ini
Tile cache:    ~/.local/share/aowis-server-map/maptiles/
```

The service unit in `configs/systemd/aowis-server-map.service` uses:

```text
Configuration: /etc/aowis-server-map/map-server.ini
Tile cache:    /var/cache/aowis-server-map/maptiles/
```

The systemd installer generates separate random API keys on first deployment, stores them in the configuration, and prints them for the administrator. The unit requires both keys and refuses to start if either is missing.

Installation instructions are in `configs/systemd/README.md`.

## Apache

Two alternative reverse-proxy templates are provided:

```text
configs/apache/aowis-map.localhost.conf   Same-host HTTP setup
configs/apache/aowis-server-map.conf      Internet-facing Certbot bootstrap setup
```

The public template starts as a valid port-80 site. Running `certbot --apache --redirect` obtains the Let's Encrypt certificate, creates the HTTPS virtual host and installs the HTTP-to-HTTPS redirect. It supports a local or remote private-network backend and uses an explicit browser-origin allowlist. Instructions are in `configs/apache/README.md`.
