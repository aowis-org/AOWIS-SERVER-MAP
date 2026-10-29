# Configuration

AOWIS Server Map uses built-in defaults, an optional administrator INI, and a separate persistent catalog for raster-provider edits made through the GUI/API. **Normal startup and configuration validation never rewrite the administrator INI.**

## Locate and validate configuration

Configuration is applied in this order:

1. Built-in defaults.
2. An INI file, if available or explicitly supplied.
3. An API-managed raster-provider catalog, if previously saved. **Only** the raster-provider list is replaced.
4. Explicit supported command-line overrides.

An ordinary user launch reads `~/.local/share/aowis-server-map/map-server.ini` when present; a missing file is normal. The supplied systemd service explicitly supplies `/etc/aowis-server-map/map-server.ini` and requires configured read and delete API keys. An explicit `--config` must name an existing valid INI.

```bash
./build-linux/aowis-server-map --print-default-config
./build-linux/aowis-server-map --write-default-config ./map-server.ini
./build-linux/aowis-server-map --check-config --config ./map-server.ini
./build-linux/aowis-server-map --config ./map-server.ini
```

`--write-default-config` refuses to overwrite an existing file. Pass `--overwrite` **only when replacing it intentionally**. The generated file is the authoritative example for the binary you have built; it includes the built-in raster providers and their UUIDs.

### Command-line options

```text
-c, --config <path>                  Load an explicit INI
--print-default-config              Print built-in default INI and exit
--write-default-config <path>       Write a new default INI and exit
--overwrite                         Permit overwriting with --write-default-config
--check-config                       Validate effective settings and exit
-a, --listen-address <address>      Override listening IP
-p, --port <port>                   Override TCP port
--max-pending-requests <count>      Override pending HTTP request limit
--max-active-downloads <count>      Override raster download concurrency
--cache-directory <path>            Override raster cache root
--api-key <key>                     Override read API key
--delete-api-key <key>              Override cache-management API key
--require-api-key                   Refuse startup without read key
--require-delete-api-key            Refuse startup without delete key
-h, --help                          Show help
--version                           Show version
```

## INI keys and defaults

| Section/key | Default | Purpose |
| --- | --- | --- |
| `server/listen_address` | `127.0.0.1` | Bind address; prefer loopback or a protected private address |
| `server/port` | `8122` | HTTP listener port |
| `server/max_pending_requests` | `2048` | Pending HTTP response/waiter ceiling |
| `downloads/remote_fetch_enabled` | `true` | Enable missing **raster** tile downloads |
| `downloads/max_active_downloads` | `32` | Active raster upstream downloads |
| `downloads/cache_directory` | empty | Raster/cache root; runtime default if blank |
| `downloads/coverage` | empty | Raster download-coverage profile; empty means unrestricted |
| `terrain/enabled` | `true` | Enable terrain HTTP subsystem |
| `terrain/remote_fetch_enabled` | `true` | Download missing **terrain** native sources |
| `terrain/default_dataset` | `copernicus-glo30` | Default point-elevation/analysis dataset |
| `terrain/maximum_jobs` | `128` | Global queued/running terrain job budget |
| `terrain/maximum_workers` | `4` | Cached/CPU terrain worker pool |
| `terrain/maximum_remote_workers` | `2` | Remote-capable acquisition worker pool |
| `terrain/connect_timeout_seconds` | `30` | Copernicus connection/initial response deadline |
| `terrain/inactivity_timeout_seconds` | `120` | Maximum network inactivity during transfer |
| `terrain/download_deadline_seconds` | `1800` | Absolute Copernicus transfer deadline |
| `terrain/cache_directory` | empty | Terrain root; defaults below raster cache root |
| `terrain/coverage` | empty | Terrain download-coverage profile |
| `storage/permanent_data_directory` | empty | Optional administrator-owned permanent data root |
| `storage/provider_state_file` | empty | Optional override for writable GUI-managed provider catalog |
| `regions/geojson_file` | empty | Optional named Polygon/MultiPolygon regions |
| `authentication/api_key` | empty | Read API key; empty disables read-key checks in user mode |
| `authentication/delete_api_key` | empty | Cache/delete API key; empty **disables** management DELETE routes |

`[terrain_provider.copernicus]` accepts `enabled`, `remote_fetch_enabled` and optional `coverage`. Provider-level settings cannot override the global terrain download switch. The remote-capable job-admission ceiling is half the global terrain job limit (64 by default); it is separate from the worker count. Other fixed in-memory budgets are documented in [Architecture](architecture.md).

Explicit relative paths in the INI resolve relative to the **INI file's directory**. Blank cache/state paths choose systemd `CACHE_DIRECTORY`/`STATE_DIRECTORY` when present, or user data locations otherwise. A blank terrain cache directory defaults to `<raster cache>/terrain`.

## Raster provider catalog

Raster provider identifiers are **stable UUIDs**, not display names. The default catalog is:

| Provider | UUID | Default zoom range |
| --- | --- | --- |
| OpenStreetMap | `0381bc44-67e9-5af9-aeb0-023ccba36658` | 0–19 |
| CyclOSM | `49bda1b9-15aa-55e6-8933-2172c785e775` | 0–20 |
| OpenTopoMap | `178a60e6-178a-507b-af86-f45427150986` | 0–17 |
| ArcGIS World Imagery | `a0d790c2-f231-55fd-b95e-23c9bd6cb3fc` | 0–17 |

These conservative defaults are configurable, not guarantees of upstream imagery availability at every location or zoom. Providers enforce their configured range. A GUI may magnify a tile or use a per-user fallback; the server does **not** choose fallback imagery providers.

One provider section looks like:

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

`{z}`, `{x}` and `{y}` placeholders are required, in any order; `{s}` optionally selects subdomains `a`, `b`, `c`. An empty provider `coverage` inherits the raster default. A provider's `remote_fetch_enabled=true` cannot override `downloads/remote_fetch_enabled=false`.

If **any** `[map_provider.*]` sections are present, they form the entire INI provider catalog; built-ins are not silently merged in. If no such sections exist, the built-in catalog applies. An API-saved empty provider list is also meaningful and remains empty.

### GUI/API provider editing

`GET /map/v1/providers` returns the configured list, including disabled providers and full upstream URL templates. `PUT /map/v1/providers` replaces that list in memory and atomically saves it to an application-managed `providers.ini` (maximum 100 providers). See the [API reference](http-api.md#raster-provider-management) for the JSON schema.

| Service path | Ownership |
| --- | --- |
| `/etc/aowis-server-map/map-server.ini` | Administrator; never rewritten by the API |
| `/var/lib/aowis-server-map/providers.ini` | GUI/API provider state; replaces only the raster provider list |
| `/var/cache/aowis-server-map/` | Disposable raster/terrain caches |
| `/var/lib/aowis-server-map/data/` | Optional administrator-supplied permanent data |

To return to the INI-defined raster provider list, stop the service, back up/remove `providers.ini`, and restart. To relocate it, use `storage/provider_state_file` pointing to a **service-writable location outside `/etc`**. Editing providers does not alter region definitions, API keys or terrain configuration. A changed upstream URL gets a separate raster cache variant using its URL fingerprint.

**Security boundary:** `PUT /map/v1/providers` currently **does not validate read or delete API keys**. Provider URLs are metadata, not secret storage. Limit PUT access to trusted users with an actual proxy/network policy; CORS headers are not access control.

## Geographic download coverage

A coverage profile controls whether a **new upstream request** is permitted. Existing raster tiles and terrain sources remain readable even outside configured coverage. There is no background regional prefetch.

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

In this example, zooms 0–6 permit world overviews, 7–11 permit the broad rectangles, and 12–30 permit only the smaller sites. Each `zoom_N` applies until the next tier. Profiles start at `zoom_0`, and a comma-separated list means **union** of regions. `world` is built in. `bbox` entries use WGS84 `west,south,east,north` longitude/latitude degrees; a region may contain multiple rectangles using `bbox`, `bbox_1`, `bbox_2`, etc. Split antimeridian-crossing rectangles into one on each side of ±180°.

An XYZ tile is eligible if **any part** of its footprint intersects an allowed region. This check does not clip Copernicus native source rasters: generating an eligible tile can require a complete 1° × 1° source COG that extends beyond the requested tile. Raster and terrain coverage profiles can differ, as can individual raster-provider overrides and the Copernicus provider override.

### Polygon country regions

A `FeatureCollection` of GeoJSON Polygon/MultiPolygon features can define named regions via an `ISO_A2` country code or `id`:

```ini
[regions]
geojson_file=regions.geojson

[coverage.projects]
zoom_0=world
zoom_7=TD,PE
```

Prepare a compact GeoJSON using an **already downloaded** Natural Earth GeoJSON input:

```bash
python3 tools/build_regions.py natural-earth-countries.geojson regions.geojson
```

`build_regions.py` uses the Python standard library and accepts GeoJSON or a ZIP containing it; shapefile-only archives need conversion first. Split polygon rings crossing the antimeridian in the input data. Natural Earth data is available from <https://www.naturalearthdata.com/downloads/> under public-domain terms.

## Permanent data and cache storage

Configured permanent raster data can be read from `<permanent_data_directory>/maptiles/<provider-uuid>/<z>/<x>/<y>.png`. It is not deleted by the raster-cache DELETE API. Downloaded raster files normally live under `<raster cache>/maptiles/`; source COGs and normalized tiles live under the terrain cache. The `terrain/offline-packages/` location is reserved; automatic mounting and arbitrary GIS dataset import are **not supported**.

Cached raster tiles do not automatically expire, revalidate or refresh. Cache deletion is the explicit invalidation mechanism. Check upstream licensing and cache-retention obligations before enabling shared storage. For physical layouts and recovery behavior, see [Architecture](architecture.md) and [Terrain sources](terrain-providers.md).
