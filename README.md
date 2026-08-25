# AOWIS-SERVER-MAP
RESTful caching map tile server for [AOWIS-SERVER-GUI](https://github.com/aowis-org/AOWIS-SERVER-GUI).

**IMPORTANT:**

This server uses QHttpServer and must not be exposed directly to the public Internet. Put it behind a TLS reverse proxy and restrict direct access to the map-server port by firewall.

By default, the server listens only on `127.0.0.1:8122`. When the reverse proxy runs on another machine, bind the map server to a private-network address and permit port 8122 only from the proxy host. A second reverse proxy on the map-server machine is unnecessary.

## Build

### Linux

On Debian or a Debian-based distribution, install the required compiler, CMake, Qt 6 base, WebSockets and HttpServer development packages plus libtiff and build without starting the server:

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

### Debian package

Build a native Debian package, including the binary, systemd service, default system configuration and disabled Apache site templates:

```bash
./compile_deb.sh --install-dependencies
```

The package is written to `dist/`. Later builds can omit dependency installation:

```bash
./compile_deb.sh --clean
```

Install the result with APT so runtime dependencies are resolved:

```bash
sudo apt install ./dist/aowis-server-map_<version>_<architecture>.deb
```

On first installation, the package creates the `aowis-server-map` service account, generates separate read and delete API keys in `/etc/aowis-server-map/map-server.ini`, enables and starts `aowis-server-map.service`, and prints the generated keys.

The Apache templates are installed directly as disabled sites:

```text
/etc/apache2/sites-available/aowis-map.localhost.conf
/etc/apache2/sites-available/aowis-server-map.conf
```

The package enables the required Apache modules but deliberately enables neither site. Edit and enable exactly the template matching the deployment.

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

[terrain]
enabled=true
remote_fetch_enabled=true
default_dataset=copernicus-glo30
cache_directory=

[authentication]
api_key=
delete_api_key=
```

`api_key` protects `/status`, raster tile GET requests, normalized terrain tile GET requests and point-elevation GET requests. An empty value disables read authentication in normal user mode.

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

Use the normal key for `/status`, raster tiles and terrain tiles. Use the delete key only for the cache DELETE endpoint.

## User mode and systemd mode

A normal user launch without `--config` uses:

```text
Configuration: ~/.local/share/aowis-server-map/map-server.ini
Map tile cache: ~/.local/share/aowis-server-map/maptiles/
Terrain cache:  ~/.local/share/aowis-server-map/terrain/
```

The service unit in `configs/systemd/aowis-server-map.service` uses:

```text
Configuration: /etc/aowis-server-map/map-server.ini
Map tile cache: /var/cache/aowis-server-map/maptiles/
Terrain cache:  /var/cache/aowis-server-map/terrain/
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

## Terrain and elevation

Terrain/elevation support is a separate subsystem inside this map server. It is deliberately independent of `MapTiles`: raster imagery and terrain have separate storage/provider logic while sharing one process, authentication surface and top-level cache root. Point elevation queries used by entity inspectors and heightfield tiles used by the native 3D renderer will share the same normalized terrain store and provider metadata.

The subsystem is configured with:

```ini
[terrain]
enabled=true
remote_fetch_enabled=true
default_dataset=copernicus-glo30
cache_directory=
```

`enabled=false` disables terrain/elevation without affecting raster map tiles. `remote_fetch_enabled=false` is strict offline-only mode: local normalized/provider cache data remains usable, but missing terrain is never fetched from the Internet.

The first remote provider is Copernicus GLO-30. Request dataset `copernicus-glo30` to use the public anonymous AWS COG distribution. The provider records EGM2008 as the vertical datum and 30 m as the nominal source resolution. Copernicus DEM is a DSM, so vegetation/buildings/infrastructure may influence heights. Remote normalization currently supports XYZ zooms 8 through 14. Provider-native COG files are retained under `terrain/providers/copernicus/glo30/` so later operation can remain offline.

When `terrain/cache_directory` is empty, the terrain root is `<downloads cache>/terrain`. The subsystem currently prepares these independent storage areas:

```text
terrain/
├── normalized/
├── providers/
└── offline-packages/
```

The normalized terrain tile format is defined and implemented as a versioned 65×65 Web-Mercator XYZ heightfield (`.aowterrain`). Normalized files live under `terrain/normalized/v1/<dataset>/<z>/<x>/<y>.aowterrain`. `TerrainData::sampleElevation()` resolves a WGS84 point from those same cached tiles, preferring the finest available zoom and using bilinear interpolation.

The renderer-facing normalized tile endpoint is:

```text
GET /terrain/v1/<dataset>/<z>/<x>/<y>.aowterrain
```

It returns the canonical cached tile bytes as `application/vnd.aowis.terrain` only after format, CRC and cache-path metadata validation. Terrain is a persistent read-through cache: normalized `.aowterrain` is checked first, then provider-native cached DEM data, then the Internet only on a true miss when remote fetching is enabled. Matching concurrent fills are deduplicated per normalized tile and per provider-native source COG instead of globally serializing unrelated terrain requests. Corrupt normalized tiles are automatically regenerated when source data are available and are never served as valid renderer data. With remote fetching disabled, missing tiles return `404`; cached provider-native data can still be normalized while fully offline. Upstream network failures return `502`.

The point-elevation endpoint used by GUI inspectors is:

```text
GET /terrain/v1/elevation?latitude=<degrees>&longitude=<degrees>[&vertical_datum=native|wgs84-ellipsoid|egm96|egm2008]
```

The server selects the dataset via `terrain/default_dataset`, so desktop/WASM clients do not know or bypass the terrain provider. Point sampling uses the same normalized tile retrieval/read-through path as the renderer. Consequently an inspector lookup first uses normalized cache data, can normalize from provider-native cached COGs while offline, and only reaches the Internet on a true miss when remote fetching is enabled. The response includes elevation, dataset, nominal source resolution, vertical datum, vertical-reference kind, authority code when globally defined, source datum, origin and the normalized tile address actually sampled. An omitted/`native` `vertical_datum` returns the source datum. Cross-datum requests are currently rejected explicitly because no geoid transformation model is bundled; AOWIS never applies an implicit constant offset. Both terrain GET routes run provider work outside the main HTTP event loop and use the existing read API key.

See [`docs/terrain-tile-format.md`](docs/terrain-tile-format.md), [`docs/terrain-elevation-contract.md`](docs/terrain-elevation-contract.md), and [`docs/terrain-providers.md`](docs/terrain-providers.md).
