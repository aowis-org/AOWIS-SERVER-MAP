# AOWIS Server Map

**AOWIS Server Map** is the HTTP map imagery and terrain-data service for
[AOWIS Server GUI](https://github.com/aowis-org/AOWIS-SERVER-GUI). It serves
Web-Mercator XYZ raster tiles, caches upstream responses, publishes an editable
raster-provider catalog, and supplies normalized terrain heightfields and point
elevations. Map imagery and terrain have **independent download controls**.

It can be run by one user, as an office cache, or as a centrally installed Linux
service. Locally available data remains available even when upstream downloads
are disabled or geographically restricted.

## Contents

- [Build, run and test](#build-run-and-test)
- [Performance and concurrency](#performance-and-concurrency)
- [Configuration and command line](#configuration-and-command-line)
- [Provider management](#provider-management)
- [Geographic download coverage](#geographic-download-coverage)
- [Caching and permanent datasets](#caching-and-permanent-datasets)
- [Terrain and elevations](#terrain-and-elevations)
- [HTTP API](#http-api)
- [Linux service and reverse proxy](#linux-service-and-reverse-proxy)

## Build, run and test

The Linux build uses a C++ compiler, CMake, Ninja, Qt 6 (Core, Network,
WebSockets, HttpServer and Concurrent), and libtiff. On Debian-based systems:

```bash
./compile_linux.sh --install-dependencies --no-run
./compile_linux.sh
```

The first command installs build dependencies and compiles without starting the
service; the second builds and starts it. For everyday development and regression testing, use the project-root test
runner. It builds the test suite **and production server** and then runs the
registered native tests:

```bash
./compile_linux_tests.sh --debug
```

Use `--release` for release-mode validation and performance work, or `--both`
for major changes and release qualification. To build the ordinary executable
without attempting to bind its listener:

```bash
./compile_linux.sh --no-run
```

The executable is `build-linux/aowis-server-map`. On non-Debian Linux systems,
install equivalent development packages and use `./compile_linux.sh --no-run`.
Arguments not recognized by `compile_linux.sh` are forwarded to the server;
`--` separates server options when needed.

An ordinary run, without a configuration file, uses built-in settings and listens
on `127.0.0.1:8122`. It does not create or update any configuration file.
With the server running and no read API key configured, a local smoke test is:

```bash
curl http://127.0.0.1:8122/status
curl http://127.0.0.1:8122/map/v1/providers
```

Use `-H 'X-API-Key: <read-key>'` when the read API key is configured.
Windows and macOS are not officially supported but may be built as Qt applications.

### Debian package

```bash
./compile_deb.sh --install-dependencies
sudo apt install ./dist/aowis-server-map_<version>_<architecture>.deb
```

Packages are built in `dist/`; subsequent builds may use
`./compile_deb.sh --clean`. The package installs the binary and systemd unit,
creates a dedicated service account, and creates the initial `/etc` configuration
with independent randomly generated read and delete API keys **only when the
configuration does not already exist**. It does not overwrite an existing
administrator configuration. Apache site templates are installed disabled.
See [Debian service instructions](configs/systemd/README.md) and
[Debian package notes](debian/aowis-server-map.README.Debian).

## Performance and concurrency

The server keeps QHttpServer's asynchronous response model. Map imagery uses a
256 MiB memory cache, asynchronous cold-cache disk reads, asynchronous bounded
atomic writes, and deduplicated upstream requests. Terrain requests use a
dedicated worker pool with explicit admission, shared in-flight jobs for
identical terrain tile, elevation and grid requests, and a separate 64 MiB
estimated grid-output reservation budget.

Default resource controls include two cache-read and two cache-write workers,
64 pending operations and 64 MiB pending payloads in each cache-I/O domain,
32 active raster downloads, four terrain workers and 128 admitted terrain
jobs. These are **independent limits**, not a promise of fixed throughput.
Blocking Copernicus source downloads can still occupy terrain workers; they
are not yet handled by a separate network I/O executor.

For poor connections, Copernicus acquisition has configurable connection,
network-inactivity and total-duration limits (30 seconds, 120 seconds and
30 minutes by default). Complete cached data remains usable offline. Interrupted Copernicus source downloads can resume with validated HTTP Range
requests when the upstream supplies a strong ETag and known total length.

See [Performance, concurrency and operations](docs/performance-roadmap.md) for
the current execution model, admission behavior, testing, limitations, and
future work. That document describes the current system, not individual patch
history.

## Configuration and command line

Configuration is resolved in this order:

1. Built-in defaults.
2. An INI file, if one is available or explicitly selected.
3. A persistent API-managed raster-provider catalog, if saved previously (it
   replaces **only** the raster-provider list).
4. Explicit CLI overrides for the supported server, cache and key settings.

Without `--config`, an ordinary user launch reads
`~/.local/share/aowis-server-map/map-server.ini`, if present. A missing default
file is normal. The supplied systemd service explicitly loads
`/etc/aowis-server-map/map-server.ini` instead. Passing `--config` requires the
named file to exist and be valid; the program does not implicitly read `/etc`
for an ordinary user launch.

For a build-tree binary, the following commands work without starting a server:

```bash
# View the complete default INI, including built-in provider UUIDs.
build-linux/aowis-server-map --print-default-config

# Generate a new INI (refuses to overwrite an existing one).
build-linux/aowis-server-map --write-default-config ./map-server.ini

# Validate the configuration used by an ordinary user launch.
build-linux/aowis-server-map --check-config

# Validate a specified installation, including its GUI-managed provider state.
build-linux/aowis-server-map --check-config --config /etc/aowis-server-map/map-server.ini
```

`--write-default-config <path> --overwrite` explicitly replaces an existing file;
**never use it on an administrator file containing credentials or custom rules
unless replacement is intentional**. Neither `--check-config` nor normal startup
rewrites an INI. Generated INI comments use `;`; the reader also accepts older
files with whole-line `#` comments without changing their contents.

The main settings are:

| INI section/key | Meaning |
| --- | --- |
| `server/listen_address`, `server/port` | Bind address and TCP port (`127.0.0.1:8122` by default) |
| `server/max_pending_requests` | Limit on pending raster responses and shared terrain single-flight HTTP waiters (default 2048) |
| `downloads/cache_directory` | Root of the raster/terrain cache; empty selects the runtime default |
| `downloads/max_active_downloads` | Simultaneous raster upstream downloads (default 32) |
| `downloads/remote_fetch_enabled` | Permit **2D raster** upstream requests (default `true`) |
| `downloads/coverage` | Default raster download-coverage profile; empty is unrestricted |
| `terrain/enabled` | Expose terrain APIs (default `true`) |
| `terrain/remote_fetch_enabled` | Permit **terrain** upstream requests (default `true`) |
| `terrain/default_dataset` | Dataset for point-elevation queries (`copernicus-glo30`) |
| `terrain/cache_directory`, `terrain/coverage` | Terrain cache root and default coverage policy |
| `terrain/maximum_jobs`, `terrain/maximum_workers` | Terrain queued/running job limit (128) and execution concurrency (4) |
| `terrain/connect_timeout_seconds` | Copernicus connection/initial-response timeout (30 s) |
| `terrain/inactivity_timeout_seconds` | Copernicus transfer inactivity timeout (120 s) |
| `terrain/download_deadline_seconds` | Copernicus absolute download deadline (1800 s) |
| `storage/permanent_data_directory` | Read-only-in-practice administrator-supplied data outside disposable caches |
| `storage/provider_state_file` | API-managed provider catalog path; normally set automatically |
| `regions/geojson_file` | Optional GeoJSON definitions for named geographic regions |
| `authentication/api_key`, `authentication/delete_api_key` | Separate read and cache-management API keys |

Explicit nonempty paths in the INI are resolved relative to that INI's directory
unless absolute. A blank cache path uses systemd's `CACHE_DIRECTORY` where
present, or the user data directory otherwise. A blank provider-state path uses
systemd's `STATE_DIRECTORY` or the user data directory.

### All CLI options

```text
-c, --config <path>                  Load an explicit INI
--print-default-config              Print built-in default INI to stdout and exit
--write-default-config <path>       Create default INI and exit
--overwrite                         Allow replacement with --write-default-config
--check-config                       Validate effective configuration and exit
-a, --listen-address <address>      Set listening IP address
-p, --port <port>                   Set listening port (default 8122)
--max-pending-requests <count>      Maximum pending tile requests (default 2048)
--max-active-downloads <count>      Simultaneous raster downloads (default 32)
--cache-directory <path>            Override raster cache root
--api-key <key>                     Override read API key
--delete-api-key <key>              Override cache-management API key
--require-api-key                   Reject startup with no read key
--require-delete-api-key            Reject startup with no delete key
-h, --help                          Display CLI help
--version                           Display version
```

## Provider management

Raster providers have stable **UUIDs**. The defaults are:

| Raster provider | UUID | Zoom range |
| --- | --- | --- |
| OpenStreetMap | `0381bc44-67e9-5af9-aeb0-023ccba36658` | 0–19 |
| CyclOSM | `49bda1b9-15aa-55e6-8933-2172c785e775` | 0–20 |
| OpenTopoMap | `178a60e6-178a-507b-af86-f45427150986` | 0–17 |
| ArcGIS World Imagery | `a0d790c2-f231-55fd-b95e-23c9bd6cb3fc` | 0–17 |

Zoom limits are configurable, deliberately conservative defaults, not a
promise that useful imagery exists throughout each provider's geographic
coverage at the maximum zoom. In particular, ArcGIS imagery resolution varies
by location. A provider never downloads beyond its configured range. Higher
GUI camera zooms may magnify existing tiles or use a **per-user GUI fallback**;
the map server does not store or choose fallback providers.

A provider in the administrator INI looks like this:

```ini
[map_provider.0381bc44-67e9-5af9-aeb0-023ccba36658]
name=OpenStreetMap
url=https://tile.openstreetmap.org/{z}/{x}/{y}.png
min_zoom=0
max_zoom=19
enabled=true
remote_fetch_enabled=true
coverage=
```

URLs require `{z}`, `{x}` and `{y}`; `{s}` optionally selects a, b or c
subdomains. A URL may use any order for its placeholders (ArcGIS uses
`{z}/{y}/{x}`). The `coverage` field optionally overrides the raster default
with another named profile; empty means inherit the raster default.
`remote_fetch_enabled` cannot override a disabled global raster switch.

If the INI has no `[map_provider.*]` sections, built-in providers apply. Once
any sections exist they form the whole configured list. There is no automatic
insertion or rewriting of defaults. A catalog explicitly saved with zero
providers remains empty. Provider IDs in HTTP requests, cache paths and the GUI
are UUIDs, not display names.

### Editing through the GUI or HTTP API

The GUI's **Settings → Map Settings → 2D Map Providers** reads
`GET /map/v1/providers` and saves the whole catalog with
`PUT /map/v1/providers`. The PUT body is JSON:

```json
{
  "providers": [
    {
      "id": "0381bc44-67e9-5af9-aeb0-023ccba36658",
      "name": "OpenStreetMap",
      "url": "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
      "minimum_zoom": 0,
      "maximum_zoom": 19,
      "enabled": true,
      "remote_fetch_enabled": true,
      "coverage": ""
    }
  ]
}
```

Saving applies the list immediately and atomically persists it in the
**application-managed** `providers.ini`. The administrator INI is never
rewritten by this API. When a provider URL changes, its raster disk cache is
separated using an upstream-URL fingerprint to avoid silently reusing imagery
from the old URL.

The GET response includes configured provider URLs and the same fields shown
above, **including disabled providers**; these are server metadata, not hidden
secrets. Keep paid-service credentials in an appropriate server-side deployment
rather than assuming this catalog conceals them.

The provider PUT endpoint currently has **no API authentication**. Network
access must be limited to trusted clients; a reverse proxy and CORS headers
alone do not authenticate writes. The GET endpoint uses the read API key when
one is configured. Per-user fallback choices belong to each GUI user's own
settings, not the server provider catalog.

### Configuration ownership in Linux service mode

| Location | Role | Managed by |
| --- | --- | --- |
| `/usr/share/aowis-server-map/map-server.ini` | Packaged example | Package |
| `/etc/aowis-server-map/map-server.ini` | Admin settings, credentials, regions and provider starting list | Administrator |
| `/var/lib/aowis-server-map/providers.ini` | Saved raster-provider catalog (optional) | Provider PUT API |
| `/var/lib/aowis-server-map/data/` | Optional persistent, manually provisioned datasets | Administrator |
| `/var/cache/aowis-server-map/` | Disposable raster and terrain caches | Running service |

The map server loads `/etc` first, then the optional state file, whose provider
list takes precedence over `/etc` provider sections **only**. Region and coverage
references in the saved list must still resolve against `/etc`. To return to
the INI-defined provider list, stop the service, back up and remove
`/var/lib/aowis-server-map/providers.ini`, then restart. Neither startup nor
package upgrades recreate this file. For alternative layouts, set
`storage/provider_state_file` to a service-writable path **outside `/etc`**.

The packaged systemd unit provisions state/cache directories and retains
`ProtectSystem=strict`. See the [systemd guide](configs/systemd/README.md).

## Geographic download coverage

Coverage rules govern **only new upstream acquisition**. The server will
happily serve a tile or terrain source already available locally, even outside
its allowed download regions. There is no background prefetching or automated
bulk download.

Define a few tiers instead of specifying all 31 supported zoom levels. Each
`zoom_N` tier applies until the next tier starts (or through zoom 30). A
profile must start at `zoom_0`. At any tier, a comma-separated list of named
regions forms a union; this supports projects in distant countries.

```ini
[downloads]
remote_fetch_enabled=true
coverage=projects

[terrain]
enabled=true
remote_fetch_enabled=false
coverage=projects

[region.chad]
bbox=13.5,7.2,24.1,23.5

[region.peru]
bbox=-81.4,-18.4,-68.6,0.0

[region.chad-site]
bbox=14.10,11.80,15.10,12.60

[region.peru-site]
bbox=-76.30,-12.40,-76.00,-12.10

[coverage.projects]
zoom_0=world
zoom_7=chad,peru
zoom_12=chad-site,peru-site
```

This example permits world overview tiles at zooms 0–6, the two broad country
rectangles at zooms 7–11, and the two site rectangles at zooms 12–30. The
rectangles are *illustrative bounds*, not exact national boundaries. A tile is
eligible if **any part of its footprint intersects** an allowed area; a tile
crossing a boundary is not excluded. Configure multiple rectangles in one
region using `bbox`, `bbox_1`, `bbox_2`, etc.

Coordinates for `bbox` are WGS84 longitude/latitude degrees in
`west,south,east,north` order. For antimeridian-spanning bounds, define
rectangles on each side of ±180°. `world` is a built-in region. An empty
`coverage=` permits unrestricted fetching, provided the relevant subsystem and
provider download switches are enabled.

Raster and terrain have **separate** `remote_fetch_enabled` and `coverage`
settings. Disable `terrain/enabled` to turn off the entire terrain API;
otherwise disable only `terrain/remote_fetch_enabled` to keep terrain APIs and
local data usable without downloading elevation sources. Each raster provider
may override its default coverage. The Copernicus provider can also specify a
profile via `[terrain_provider.copernicus]`; this does not override the global
terrain download switch.

### Named country boundaries (optional)

A map server can load GeoJSON Polygon/MultiPolygon regions from a
`FeatureCollection` using `ISO_A2` country codes or an `id` property:

```ini
[regions]
geojson_file=regions.geojson

[coverage.projects]
zoom_0=world
zoom_7=TD,PE
zoom_12=chad-site,peru-site
```

The relative GeoJSON path is resolved next to the administrator INI. To prepare
country data from **a locally available Natural Earth GeoJSON file**:

```bash
python3 tools/build_regions.py natural-earth-countries.geojson regions.geojson
```

The script requires only Python 3, performs no network requests, and also
accepts a ZIP containing a GeoJSON file. Shapefile-only archives must first be
converted to GeoJSON with an external GIS tool. Natural Earth data is available
at <https://www.naturalearthdata.com/downloads/> under public-domain terms.
The generated file can be copied to an offline server. GeoJSON polygon rings
crossing ±180° should be split in the source.

**Terrain source granularity:** an eligible normalized terrain request may need
an entire 1°×1° Copernicus COG, which can extend outside the permitted request
tile; the restriction controls which requested tile may trigger the download,
not geographic clipping of provider-native source rasters.

## Caching and permanent datasets

### Raster cache

Raster map tiles are **retained indefinitely** once successfully cached. Memory,
disk, and permanent-data hits are served immediately, including while online;
there is no time-based invalidation, background refresh, HTTP conditional
revalidation, or expiration-driven download. Upstream requests occur **only
for missing tiles**, when 2D downloads are enabled and the provider's zoom and
geographic download policy permits the request. Existing `.http.json` freshness
sidecars from older installations are ignored; cached image files remain valid
and do not need conversion or deletion. The explicit cache-delete API is how
administrators remove cached tiles to allow fresh copies to be fetched.

Responses carrying explicit `Cache-Control: no-store` or `private` directives
are delivered to the requesting client but are not persisted in the shared
cache. Check the licensing and caching requirements of your chosen providers;
retaining cached tiles indefinitely may not be suitable for every third-party
service. Terrain continues to use separate native-source and normalized-heightfield
caches and has its own independent upstream download switch.

Missing tiles outside a
provider's zoom range or allowed coverage are not downloaded. A successful
HTTP response containing an image of an upstream error cannot always be
identified automatically, which makes accurate provider zoom limits important.
Public map services have their own attribution, request and cache rules; respect
each provider's published usage policy.

Raster cache paths use the UUID:

```text
<cache root>/maptiles/<provider-uuid>/<z>/<x>/<y>.png
```

Changed upstream URLs may use a separate fingerprinted variant directory
under the provider's cache tree. The map cache DELETE API does **not** remove
permanent raster files. There is no general cache-size quota or cache-eviction
policy controlled by the geographic coverage configuration.

### Permanent local data

```ini
[storage]
permanent_data_directory=/var/lib/aowis-server-map/data
```

Manually provisioned raster tiles can be read from
`<permanent_data_directory>/maptiles/<provider-uuid>/<z>/<x>/<y>.png`.
The corresponding `terrain/` area is reserved for offline terrain packages
outside disposable caches; **automatic mounting of offline packages is not
implemented**. The server does **not** automatically import arbitrary GIS file
formats or download complete regions. Without an explicit permanent directory,
the offline-package directory is placed under the terrain cache.

## Terrain and elevations

Terrain is a separate subsystem: `terrain/enabled=false` disables its routes
without affecting raster tiles, and `terrain/remote_fetch_enabled=false` blocks
only new upstream terrain data downloads. The supported Internet provider is
**Copernicus GLO-30** (`copernicus-glo30`), retrieved from publicly accessible
COGs. It is a **digital surface model (DSM)** with nominal 30 m resolution and
an **EGM2008** vertical datum; elevations may include vegetation and structures
and must not be confused with surveyed hydraulic heights.

Terrain stores provider-native data and normalized heightfields separately:

```text
<terrain cache>/
├── providers/copernicus/glo30/           # native source COGs
├── normalized/v1/copernicus-glo30/      # z/x/y.aowterrain
└── offline-packages/
```

A normalized tile is a validated 65×65 Web-Mercator heightfield (`.aowterrain`)
with a versioned binary structure and CRC. Copernicus on-demand normalization
supports zooms **8–14**. Point sampling and 3D tile delivery share the same
read-through store. On a normalized cache miss, the server uses already-cached
native COGs if possible, and only fetches missing COGs when terrain upstream
fetching is allowed. Concurrent requests for the same tile or source COG are
deduplicated. Unsupported datum transformations are **rejected**, not silently
approximated; native EGM2008 elevation is not automatically converted to WGS84
ellipsoid height.

Technical references:

- [Terrain providers and caching](docs/terrain-providers.md)
- [Terrain/elevation API and datum contract](docs/terrain-elevation-contract.md)
- [Normalized `.aowterrain` binary format](docs/terrain-tile-format.md)

## HTTP API

All paths below are relative to the configured server address and port.
Provider IDs are UUIDs; terrain dataset IDs such as `copernicus-glo30` are
not provider UUIDs.

| Method and route | Purpose |
| --- | --- |
| `GET /status` | Plain-text server and terrain status |
| `GET /map/v1/providers` | Configured raster provider catalog, including URL and zoom limits |
| `PUT /map/v1/providers` | Replace and persist raster provider catalog immediately |
| `GET /<provider-uuid>/<z>/<x>/<y>.png` | 2D raster tile (read-through cache) |
| `DELETE /cache/<provider-uuid>/<z>/<x-min>/<x-max>/<y-min>/<y-max>` | Delete raster cache tiles in XYZ range |
| `GET /upstream/v1/activity` | Active/queued raster and terrain upstream requests |
| `DELETE /upstream/v1/map-tiles` | Cancel raster upstream downloads |
| `DELETE /upstream/v1/terrain` | Cancel terrain upstream downloads |
| `GET /terrain/v1/datasets` | Terrain catalog with zoom, datum and source metadata |
| `GET /terrain/v1/<dataset>/<z>/<x>/<y>.aowterrain` | Validated normalized terrain tile |
| `GET /terrain/v1/elevation?latitude=…&longitude=…` | Point elevation; optional `vertical_datum` query |
| `GET /terrain/v1/analysis/grid?dataset=…&west=…&south=…&east=…&north=…&columns=…&rows=…` | Native-precision elevation window for Hydrology |
| `DELETE /terrain/v1/cache/<dataset>/<z>/<x-min>/<x-max>/<y-min>/<y-max>` | Delete normalized terrain tiles |
| `DELETE /terrain/v1/source/<dataset>/<z>/<x-min>/<x-max>/<y-min>/<y-max>` | Delete corresponding provider-native terrain source cells |

The map tile endpoint also accepts `?delivery=cache-first` (or the
`X-AOWIS-Tile-Delivery: cache-first` header): if the tile is being fetched it
may return HTTP **202** rather than holding the request pending. Otherwise a
cache miss eligible for upstream fetch may wait for the result. Invalid tile
requests, including missing tiles whose downloads are disallowed, currently
return HTTP **400**. A full request queue returns HTTP **503**.

Terrain `cache` deletion removes normalized tiles, which can later be
regenerated from native source data. Terrain `source` deletion removes the
relevant cached 1° Copernicus COG cells. A source cell may be shared by tiles
outside the selected XYZ range. These operations require the delete API key.

### API-key behavior

The read key (`authentication/api_key`) is used for status, catalogs, activity,
map tiles and terrain GET routes. The separate delete key
(`authentication/delete_api_key`) is used for cache deletion and upstream
cancellation. Send either as:

```http
X-API-Key: your-endpoint-specific-key
```

or `Authorization: Bearer your-endpoint-specific-key`. In user mode, an empty
read key disables read authentication; an empty delete key **disables DELETE
management operations**, rather than making them public. The supplied systemd
unit requires both keys at startup. **The provider PUT API is currently not
covered by either key.**

## Linux service and reverse proxy

The packaged unit runs as `aowis-server-map`, reads an explicit administrator
INI and provisions writable `/var/lib` and `/var/cache` directories through
systemd. It retains `ProtectSystem=strict`, so the application does not require
write permissions under `/etc`. The manual installer places its vendor unit
under `/usr/local/lib/systemd/system`; Debian packages install it under
`/usr/lib/systemd/system`. Administrator systemd overrides belong in
`/etc/systemd/system`, created with `sudo systemctl edit aowis-server-map`.

See [systemd deployment](configs/systemd/README.md) and
[Apache reverse proxy](configs/apache/README.md) for installation, service
administration, DNS/TLS examples and configuration ownership.

**Deployment boundary:** QHttpServer is not intended as a direct public-facing
HTTP server. Run it on loopback or a private interface behind a suitable TLS
reverse proxy and restrict direct backend access. In particular, the
unauthenticated provider-editing PUT route must not be reachable by untrusted
clients. Apache examples provide CORS response headers but do not constitute
API authorization or network isolation.

## Hydrology integration: analysis-grade DEM windows

Map is the single authority for terrain providers, native Copernicus source
files, cache policy and dataset metadata. Hydrology should **not** maintain a
separate directory of Copernicus files or reimplement source downloading.

The Map C++ library now exposes:

```cpp
Aowis::Map::TerrainAnalysisGridRequest request;
request.dataset = QStringLiteral("copernicus-glo30");
request.bounds = {8.0, 50.0, 8.01, 50.01}; // west, south, east, north (WGS84)
request.columns = 32;
request.rows = 32;
const Aowis::Map::TerrainAnalysisGridResult grid = map_terrain.analysisGrid(request);
```

An HTTP deployment uses the equivalent read endpoint:

```text
GET /terrain/v1/analysis/grid?dataset=copernicus-glo30&west=8&south=50&east=8.01&north=50.01&columns=32&rows=32
```

For successful requests, the response is `application/vnd.aowis.dem-f32le`,
containing `rows * columns` **little-endian IEEE754 float32** samples in
row-major north-to-south order. Each sample is taken from the **original
Copernicus GeoTIFF pixel containing its cell centre**; neither the 65×65
renderer format nor bilinear interpolation is used. The requested grid may
be different from the native GeoTIFF resolution (nearest source-pixel
sampling); use an analysis grid aligned to the source where possible.

A request covers WGS84 longitude [-180,180], latitude [-85,85], does not cross
the antimeridian, and is limited to **512 columns × 512 rows**. For larger
regions, clients request adjacent subwindows whose cell centres exactly match
a common full-grid geometry. Missing/NoData source cells are quiet NaNs in
otherwise successful windows; a completely unavailable window returns HTTP
404. Hydrology detects incomplete input before running its flow algorithms.

Map's normal read-access policy applies to the endpoint, as it does to other
terrain reads. `TerrainData::analysisGrid()` and HTTP use the **same provider
implementation**. Copernicus GLO-30 elevations are EGM2008-referenced;
consult `/terrain/v1/datasets` for vertical-datum metadata. No datum
conversion is performed by this endpoint. Existing imagery/terrain download
controls remain independent; `terrain/remote_fetch_enabled=false` prevents
remote source retrieval while continuing to serve cached GeoTIFFs.

The in-process route is intended for [AOWIS Server Standalone](https://github.com/aowis-org/aowis-server-standalone): it should pass its existing `TerrainData`
instance to the Hydrology library, not instantiate a second Map server or use
localhost HTTP. Hydrology's analysis results and job persistence remain
Hydrology-owned. See the Hydrology repository for its REST and C++ API.
