# Configuration

AOWIS-SERVER-MAP has **two kinds of settings**. The administrator INI controls how a
server runs (network, access, storage, download limits); MAP-owned
**AOWIS-CONFIG** records hold the editable raster-provider catalog, cascade
configuration, inherited-provider enablement and per-provider routing/modes. Tile caches are data, **not configuration**. Neither startup nor
REST editing rewrites the administrator INI.

## Pick your deployment

| How MAP runs | Where you manage it | Configuration locations on Linux |
| --- | --- | --- |
| **Embedded in AOWIS-Standalone** | GUI → Settings → Map Settings; MAP C++ library underneath | MAP records: `~/.config/aowis/standalone-map/` (or `$XDG_CONFIG_HOME/aowis/standalone-map/`). **No MAP server INI.** |
| **Separate process, started in a terminal** | Optional INI for server options; MAP REST API for providers | INI: `~/.local/share/aowis-server-map/map-server.ini`; managed records: `~/.local/share/aowis-server-map/config/server-map/`; cache: `~/.local/share/aowis-server-map/` |
| **Installed systemd service** | Administrator INI in `/etc`; MAP REST API for providers | INI: `/etc/aowis-server-map/map-server.ini`; managed records: `/var/lib/aowis-server-map/config/server-map/`; cache: `/var/cache/aowis-server-map/` |

Paths are defaults, not a promise that the directories already exist. For
Standalone, `XDG_CONFIG_HOME` changes the user configuration base. For manually
started MAP, `XDG_DATA_HOME` changes the user data base. For systemd, the unit
supplies `STATE_DIRECTORY` and `CACHE_DIRECTORY`. The INI's
`storage/managed_settings_directory` can override the independent server's
managed-record root; explicit cache directories override cache defaults.

**These are separate installations of MAP:** editing Standalone's embedded MAP
settings does not edit a separate running server. A normal, non-Standalone GUI
connects to a MAP server over REST; only Standalone uses MAP's in-process C++
settings API. The GUI must not read or write MAP's configuration files itself.

## What goes where?

| Settings | Owner and change mechanism | When applied |
| --- | --- | --- |
| Listen IP/port, API keys, worker limits, cache directories, coverage regions, global raster/terrain download switches | Administrator **INI**, optional CLI overrides | Process start/restart |
| Local raster providers (name, tile URL, zoom bounds, enabled, per-provider restrictions) | **MAP-owned AOWIS-CONFIG** provider record; `GET/PUT /map/v1/providers` on a separate server, MAP library in Standalone | REST or embedded-library edits apply immediately |
| Global cascading (ordered MAP URLs/read keys, direct fallback, slow-link settings) | MAP CONFIG cascade record, or explicit **administrator INI** entries | Separate-server INI changes require restart; Standalone library changes autosave and apply live |
| Inherited-provider enablement, selected upstream names/order, raster acquisition mode | Separate MAP CONFIG records; authenticated provider REST endpoints or Standalone MAP library | Applied immediately without changing upstream definitions |
| Raster tiles, Copernicus source data, normalized terrain | **Cache**, kept independently of configuration | Reused on subsequent requests, including offline |

AOWIS-CONFIG stores versioned, revision-checked JSON records under MAP's owner
root. It is not another INI and is not intended for hand editing. MAP validates
and saves it through `Aowis::Map::SettingsRepository`. A stale provider write is
rejected with HTTP **409** instead of overwriting another editor's changes.
Provider REST editing currently **does not check API keys**: expose that write
route only to trusted clients using firewall/reverse-proxy access controls.

### Which value wins?

- Built-in defaults apply first. An available administrator INI defines operating
  settings and can define raster providers. An existing managed provider record
  **replaces the complete INI provider list**, not individual entries.
- A managed cascading record applies **unless the administrator INI explicitly
  sets any `[upstream]`, `[upstream.NAME]`, or `[bandwidth]` directive**. In that
  case the **entire** INI cascade configuration wins; fields are not merged.
- Supported CLI overrides (for example `--listen-address`) apply to that
  process. Global remote-fetch switches and coverage restrictions remain gates
  even when cascading is configured.
- Per-provider selected upstreams and acquisition modes are downstream-local
  MAP CONFIG records; they do not override administrator remote-fetch gates,
  coverage limits or the global direct-fetch prohibition. Local raster providers
  default to **Automatic** (MAP upstreams then permitted origin), inherited
  providers to **Cascade only** (never the original imagery source). **Direct**
  is allowed only for local providers when global direct fetching is enabled.
- Cache hits work with remote downloads disabled, but a disabled provider cannot
  serve even cached raster tiles. Terrain uses global upstream priority and
  `allow_direct_fetch`, not raster-specific acquisition modes.

For a restricted branch office, an administrator can pin the cascade in INI:

```ini
[upstream]
servers=primary,backup
allow_direct_fetch=false

[upstream.primary]
url=https://maps-primary.example.org
api_key=PRIMARY_READ_KEY

[upstream.backup]
url=https://maps-backup.example.org
api_key=BACKUP_READ_KEY

[bandwidth]
poor_internet_mode=true
jpeg_quality=75
```

These INI keys **replace the whole managed global cascade configuration**;
they do not merge with it. A separate MAP server has **no public REST endpoint
for changing its global upstream list**; edit the INI and restart instead.
The provider-specific REST endpoints can still change local enablement, routing
priority and acquisition mode at runtime. In Standalone, use the GUI; there is
no MAP daemon or restart requirement for GUI-managed cascade settings. See
[Cascading MAP servers](map-cascading.md) for a complete site setup and
transfer rules.

## First-run, migration and recovery

- **Standalone:** MAP owns the `standalone-map` CONFIG root and imports old
  GUI-stored embedded-MAP preferences only when the corresponding MAP record is
  absent. Old preferences are left intact for rollback.
- **Separate/systemd server:** if no managed provider record exists, MAP checks
  the old `providers.ini`, validates it and imports it **once**. The legacy file
  stays untouched. On later starts, the managed provider record wins. An
  invalid managed or legacy record causes an error instead of silently
  restoring defaults or enabling downloads.
- **`--check-config`:** validates settings without starting a listener or
  writing a migration. To inspect the systemd server's *actual* managed records,
  run it as the service user with its state/cache environment (see
  [systemd deployment](../configs/systemd/README.md#configuration-management)).
- **Backups:** stop the owner, then back up its entire CONFIG owner directory
  (records may contain upstream read keys). Back up the administrator INI
  separately. Do not treat tile-cache deletion as a settings reset.

## Start and inspect a separate MAP server

A manually started server uses built-in defaults plus any user INI and
AOWIS-CONFIG records. Without an INI, it normally listens on
`127.0.0.1:8122`. An explicit `--config` must reference a valid existing file.

```bash
./build-linux/aowis-server-map --print-default-config
./build-linux/aowis-server-map --write-default-config ./map-server.ini
./build-linux/aowis-server-map --check-config --config ./map-server.ini
./build-linux/aowis-server-map --config ./map-server.ini
```

`--write-default-config` will not replace an existing file without
`--overwrite`. The generated INI is a useful reference for the installed
binary, not a replacement for MAP's managed CONFIG records.

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

### Listen on another network interface

The default listener is `127.0.0.1:8122`, reachable **only from the same machine**. For access from another computer, bind to a suitable private IPv4 address or all IPv4 interfaces (`0.0.0.0`):

```bash
# Direct launch: listen on all IPv4 interfaces, TCP port 8122
./build-linux/aowis-server-map --listen-address 0.0.0.0 --port 8122

# Build and launch with exactly the same server options
./compile_linux.sh --listen-address 0.0.0.0 --port 8122
```

CLI flags `--listen-address` / `-a` and `--port` / `-p` override the INI **only for the current process**. For persistent configuration, specify the listener in an INI file:

```ini
[server]
listen_address=0.0.0.0
port=8122
```

For a manually started server, pass `--config ./map-server.ini` (or place the INI at its default user configuration path). For the packaged systemd service, change `/etc/aowis-server-map/map-server.ini` and run `sudo systemctl restart aowis-server-map`; it does not inherit arguments passed to a separate shell launch.

Use `ss -ltn | grep ':8122'` locally to confirm the listening socket. From another machine, use `curl http://<server-lan-ip>:8122/status`, supplying `-H 'X-API-Key: YOUR_READ_KEY'` when the read API key is set. A firewall must also permit the desired clients.

**Security:** Binding `0.0.0.0` exposes the HTTP listener on *all* IPv4 interfaces, potentially including public ones. `PUT /map/v1/providers` does not currently check API keys. Bind to a private interface where possible, restrict network access, and use a controlled TLS reverse proxy for external deployment. See [systemd deployment](../configs/systemd/README.md#local-and-remote-reverse-proxies).

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
| `storage/provider_state_file` | empty | Legacy provider catalog path, read only for one-time migration |
| `storage/managed_settings_directory` | empty | Optional CONFIG `server-map` owner root (defaults under the service state directory) |
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

String values containing a comma must be enclosed in double quotes (`name="Esri, Maxar"`); Qt's INI reader would otherwise split them into a list. The server rejects an unquoted comma in provider `name`, `url` and `coverage` and in `authentication/api_key`/`delete_api_key` instead of silently dropping the value. Inside quotes, write `\"` for a quote and `\\` for a backslash. The legacy `providers.ini` writer used this escaping; current edits are stored through AOWIS-CONFIG.

`{z}`, `{x}` and `{y}` placeholders are required, in any order; `{s}` optionally selects subdomains `a`, `b`, `c`. An empty provider `coverage` inherits the raster default. A provider's `remote_fetch_enabled=true` cannot override `downloads/remote_fetch_enabled=false`.

If **any** `[map_provider.*]` sections are present, they form the entire INI provider catalog; built-ins are not silently merged in. If no such sections exist, the built-in catalog applies. An API-saved empty provider list is also meaningful and remains empty.

### GUI/API provider editing

`GET /map/v1/providers` returns **local** definitions, including disabled
providers and URL templates. `PUT /map/v1/providers` replaces that local list,
validates it, stores it in MAP-owned AOWIS-CONFIG and applies it immediately
(maximum 100 providers). Inherited providers and their local routing state
are visible through `GET /map/v1/providers/available`; they are never written
into the editable local-provider list. See [provider management](http-api.md#raster-provider-management)
and [provider routing](http-api.md#provider-routing-and-acquisition).

| Service path | Ownership |
| --- | --- |
| `/etc/aowis-server-map/map-server.ini` | Administrator; never rewritten by the API |
| `/var/lib/aowis-server-map/config/server-map/` | MAP-managed AOWIS-CONFIG provider and cascade records |
| `/var/lib/aowis-server-map/providers.ini` | Read-only legacy provider state, imported once if present |
| `/var/cache/aowis-server-map/` | Disposable raster/terrain caches |
| `/var/lib/aowis-server-map/data/` | Optional administrator-supplied permanent data |

To return to the INI-defined raster providers, stop the service, back up/remove the managed CONFIG provider record using CONFIG's supported storage API, and restart. The legacy `providers.ini` remains a migration source until it is backed up or removed; move it out of the state directory if you want to restore administrator defaults after clearing CONFIG. Editing providers does not alter region definitions, API keys or terrain configuration. A changed upstream URL gets a separate raster cache variant using its URL fingerprint.

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
