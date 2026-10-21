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

A complete, commented INI containing all built-in map and terrain providers and a
multi-project example is available without writing anything:

```bash
./aowis-server-map --print-default-config
```

Validate the effective configuration and print the resolved provider/coverage summary:

```bash
./aowis-server-map --check-config
./aowis-server-map --check-config --config /etc/aowis-server-map/map-server.ini
```

Without `--config`, validation loads the **same default user configuration** that normal
startup would use, or validates built-in defaults if that file does not exist. It
never creates or modifies an INI file. The server never rewrites administrative
configurations on startup. **Do not use `--overwrite` unless you intend to replace
an existing config and its credentials.**

### 2D raster and terrain downloads are independent

```ini
[downloads]
remote_fetch_enabled=true     # Only raster map tiles
max_active_downloads=32
cache_directory=
coverage=

[terrain]
enabled=true
remote_fetch_enabled=false    # No new elevation source downloads
cache_directory=
coverage=
```

`terrain/enabled=false` disables the terrain API entirely. Set only
`terrain/remote_fetch_enabled=false` to keep existing cached elevation data available
without Internet access. Neither setting changes 2D raster tile downloads.
Conversely, setting `downloads/remote_fetch_enabled=false` never disables remote
terrain downloads. Already-cached or permanently supplied raster data can always
be served independently of the download policy. Cached normalized terrain tiles
and provider-native local source rasters work when terrain remote fetching is off.

### Multi-region, zoom-dependent download coverage

A **coverage profile** is a short list of zoom starting points. Each step remains
effective until the next one; the last continues through zoom 30. The intersection
of the actual XYZ tile footprint with **any** named region permits acquisition.
The regions may be on different continents. All locally available data is served
regardless of the download profile.

```ini
[downloads]
remote_fetch_enabled=true
coverage=water-projects

[terrain]
enabled=true
remote_fetch_enabled=false
coverage=water-projects

[region.chad]
bbox=13.5,7.2,24.1,23.5

[region.peru]
bbox=-81.4,-18.4,-68.6,0.0

[region.chad-site]
bbox=14.10,11.80,15.10,12.60

[region.peru-site]
bbox=-76.30,-12.40,-76.00,-12.10

[coverage.water-projects]
zoom_0=world
zoom_7=chad,peru
zoom_12=chad-site,peru-site
```

`bbox=west,south,east,north` uses WGS84 longitude/latitude degrees. These illustrative
country bounding rectangles intentionally include areas outside the countries; to
use precise country borders, prepare a GeoJSON boundary dataset as described below.
A region can contain more than one rectangle using `bbox_1`, `bbox_2`, etc. Use this
for disjoint areas or to split an antimeridian-crossing area into two rectangles.

The example allows the full world at zooms 0–6, broad coverage over **both** projects
at zooms 7–11, and only the two smaller project areas from zoom 12 onward. A requested
tile **may intersect a permitted region at its edge**: the entire tile remains
eligible, so border coverage is not lost. These rules do not prefetch anything;
they apply only when users request currently missing data. An empty `coverage=` means
unrestricted downloads (subject to the corresponding remote-fetch switch).

The raster and terrain subsystems can name different profiles. There is no system-wide
master switch coupling them together. Per-provider coverage can replace the default
for that provider, but its `remote_fetch_enabled` cannot bypass its subsystem's
`remote_fetch_enabled=false` switch.

### Configuring raster and terrain providers

The default raster providers are `openstreetmap`, `osmcyclo`, `opentopomap` and
`arcgis`. The supported terrain provider is `copernicus` (dataset
`copernicus-glo30`). The complete default INI lists all of them. If no
`[map_provider.*]` sections are present, the server uses the built-in raster list
without modifying the file. **Once at least one such section exists, those sections
are the complete explicit list**; deliberately omitted providers are not restored.

```ini
[map_provider.openstreetmap]
name=OpenStreetMap
enabled=true
remote_fetch_enabled=true
url=https://tile.openstreetmap.org/{z}/{x}/{y}.png
min_zoom=0
max_zoom=19
# Optional replacement of [downloads] coverage:
coverage=

[terrain_provider.copernicus]
enabled=true
remote_fetch_enabled=true
# Optional replacement of [terrain] coverage:
coverage=
```

Raster URLs support `{z}`, `{x}`, `{y}` and optional `{s}` (one of `a`, `b`, `c`)
for conventional XYZ maps. ArcGIS URLs can reverse the order of `{x}` and `{y}`.
`enabled=false` removes a provider from serving and catalog discovery. A raster
provider's maximum zoom controls new upstream downloads only; existing local tiles
at other zooms remain available. The server does not automatically prefetch an
entire area. A public map provider may impose separate fair-use, request rate,
attribution and commercial caching rules; the administrator must follow them.

`GET /map/v1/providers` returns the enabled raster provider IDs, names and zoom
ranges without exposing upstream URLs or credentials. `GET /terrain/v1/datasets`
continues to report the terrain dataset catalog. The existing GUI has some legacy
hardcoded raster-layer choices; this API is available for a later GUI-side update.

### Offline geographic reference data

For precise country boundaries, use a Natural Earth GeoJSON `FeatureCollection`
with `ISO_A2` country identifiers, or a custom GeoJSON with an `id` string property.
Both Polygon and MultiPolygon geometries are supported. GeoJSON is loaded on startup
only when explicitly configured:

```ini
[regions]
geojson_file=regions.geojson

[coverage.world-and-chad]
zoom_0=world
zoom_7=TD
```

The file path is relative to the INI directory, unless absolute. Named GeoJSON
regions and manually defined `[region.*]` rectangles can be combined in the same
profile. To create a compact, redistributable file from a **locally available
Natural Earth GeoJSON** (including a ZIP containing one GeoJSON file), run:

```bash
python3 tools/build_regions.py natural-earth-countries.geojson regions.geojson
```

The tool has no network access or nonstandard Python dependencies. It can run on
another computer; copy `regions.geojson` alongside the INI for an air-gapped server.
A source ZIP containing only ESRI shapefiles must first be converted to GeoJSON by a
GIS tool such as `ogr2ogr`. No country data is downloaded during server startup.
Natural Earth: <https://www.naturalearthdata.com/downloads/> (public domain).
GeoJSON rings crossing the antimeridian should be split at ±180° in the source;
for custom rectangles simply configure one on each side.

### Cache freshness and permanent storage

When **2D upstream downloads are enabled**, the raster cache honors upstream
`Cache-Control` max-age/no-store/no-cache, `Expires`, `ETag`, and `Last-Modified`
headers. Expired raster tiles can be served immediately while a conditional
revalidation runs in the background; `304 Not Modified` refreshes metadata without
downloading or replacing the image. Temporary errors leave previously cached tiles
available. Requests for the same tile are deduplicated and failed revalidations
receive a short retry delay. Where a server sends no cache lifetime, AOWIS uses a
7-day fallback. Metadata lives beside tile images as `.http.json` files.

With **2D upstream downloads disabled**, there are no remote revalidation requests:
existing files remain servable regardless of their recorded freshness. Terrain
remote fetching remains independently controlled and uses its normalized/source
caches; it is not governed by raster HTTP freshness metadata.

```ini
[storage]
permanent_data_directory=/srv/aowis-map-data
```

Permanent raster tiles are read from
`<permanent_data_directory>/maptiles/<provider>/<z>/<x>/<y>.png` and are not
removed by the raster cache-delete route. Terrain offline packages are directed to
`<permanent_data_directory>/terrain`. Leave this setting empty to preserve the
existing default offline-package directory within the terrain cache. This is a
path for administrator-managed local assets; the server does **not** automatically
import arbitrary GIS files.

`api_key` protects `/status`, raster tile GET requests, normalized terrain tile GET requests and point-elevation GET requests. An empty value disables read authentication in normal user mode.

`delete_api_key` protects cache DELETE requests. An empty delete key disables cache deletion; it never makes deletion public.

## Server options

```text
-c, --config <path>                  Load settings from this INI file
--write-default-config <path>       Write built-in defaults and exit
--print-default-config              Print complete default INI without writing
--check-config                       Check effective config and report settings
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

Use the normal key for `/status`, raster tiles and terrain tiles. Use the delete key only for the cache DELETE endpoints (raster tiles, terrain tiles and terrain source data) and the upstream-download cancel endpoints.

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

Renderers learn which terrain to request from the dataset catalog:

```text
GET /terrain/v1/datasets
```

It returns `application/json` with `default_dataset` (the configured `terrain/default_dataset`) and one entry per served dataset: `dataset`, `provider`, `nominal_resolution_m`, `vertical_datum`, `surface_model` (`dsm`, `dtm` or `unknown`), the generated zoom range `minimum_zoom`/`maximum_zoom` and `tile_grid_size`. `maximum_zoom` is the provider's native detail; renderers never request finer terrain tiles and subdivide the `maximum_zoom` tile instead, so the drawn surface never claims more detail than the source has. The route uses the read API key and returns `503` while terrain is disabled or not initialized.

The point-elevation endpoint used by GUI inspectors is:

```text
GET /terrain/v1/elevation?latitude=<degrees>&longitude=<degrees>[&vertical_datum=native|wgs84-ellipsoid|egm96|egm2008]
```

The server selects the dataset via `terrain/default_dataset`, so desktop/WASM clients do not know or bypass the terrain provider. Point sampling uses the same normalized tile retrieval/read-through path as the renderer. Consequently an inspector lookup first uses normalized cache data, can normalize from provider-native cached COGs while offline, and only reaches the Internet on a true miss when remote fetching is enabled. The response includes elevation, dataset, nominal source resolution, vertical datum, vertical-reference kind, authority code when globally defined, source datum, origin and the normalized tile address actually sampled. An omitted/`native` `vertical_datum` returns the source datum. Cross-datum requests are currently rejected explicitly because no geoid transformation model is bundled; AOWIS never applies an implicit constant offset. Both terrain GET routes run provider work outside the main HTTP event loop and use the existing read API key.

Cached terrain is deleted per zoom level and XYZ tile range, with the raster cache DELETE's range semantics (rows are clamped to the zoom level, columns wrap around the antimeridian, a range at least as wide as the zoom level covers every column):

```text
DELETE /terrain/v1/cache/<dataset>/<z>/<x_min>/<x_max>/<y_min>/<y_max>
DELETE /terrain/v1/source/<dataset>/<z>/<x_min>/<x_max>/<y_min>/<y_max>
```

`cache` removes the normalized `.aowterrain` tiles of the range; they are regenerated on the next request, from cached provider-native data when it is still there. `source` removes the provider-native source data (for Copernicus GLO-30 the 1°×1° GeoTIFF cells under `terrain/providers/copernicus/glo30/`) covering the range's geographic area and forgets any "source unavailable" state there, so regenerated tiles come from freshly downloaded source data. A source file that is being downloaded at that moment is kept (it is new data). Source cells can be shared with normalized tiles outside the range; those download the cell again when they are next regenerated. Both answer `200` with the number of deleted files, `400` for an invalid dataset or range, `503` while terrain is disabled or not initialized and `500` when a file could not be deleted. They use the delete API key; the GUI's Map Editor offers them in its *Terrain Cache* section. With `remote_fetch_enabled=false`, deleting source data makes the area unavailable until the data is restored.

See [`docs/terrain-tile-format.md`](docs/terrain-tile-format.md), [`docs/terrain-elevation-contract.md`](docs/terrain-elevation-contract.md), and [`docs/terrain-providers.md`](docs/terrain-providers.md).
