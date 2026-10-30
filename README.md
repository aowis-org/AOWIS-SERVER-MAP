# AOWIS Server Map

**AOWIS Server Map** is the map imagery and elevation service for [AOWIS Server GUI](https://github.com/aowis-org/AOWIS-SERVER-GUI). It serves XYZ raster map tiles, maintains local caches and a configurable provider catalog, and exposes Copernicus GLO-30 terrain as normalized 3D heightfields, point elevations and analysis-grade DEM grids.

It is a C++/Qt 6 HTTP server for Linux, usable as a local desktop companion, a shared office cache or a systemd-managed backend. Raster imagery and terrain have **independent remote-download and geographic-coverage controls**. Already cached data is still served when downloading is disabled.

> **Security:** The raster-provider editing endpoint (`PUT /map/v1/providers`) has **no API-key authentication**. Bind to loopback or a private interface and restrict that endpoint to trusted clients. For external access, use a TLS reverse proxy with appropriate access controls. See [Deployment](#deployment).

## Start here

Build dependencies are a C++ compiler, CMake, Ninja, Qt 6 (Core, Network, HttpServer, WebSockets and Concurrent) and libtiff. On Debian-based Linux:

```bash
./compile_linux.sh --install-dependencies --no-run
./compile_linux.sh --no-run
./build-linux/aowis-server-map
```

Without an INI file, the server uses built-in defaults and listens at **`127.0.0.1:8122`**. It does not create or modify a configuration file on startup. In a second terminal:

```bash
curl -fsS http://127.0.0.1:8122/status
curl -fsS http://127.0.0.1:8122/map/v1/providers
curl -fsS http://127.0.0.1:8122/terrain/v1/datasets
```

If a read API key is configured, add `-H 'X-API-Key: YOUR_READ_KEY'` to GET requests. `./compile_linux.sh` **without** `--no-run` builds *and starts* the server; when another instance already uses port 8122, a subsequent bind failure does not indicate a compilation failure.

Generate and validate a configuration **without starting the listener**:

```bash
./build-linux/aowis-server-map --print-default-config
./build-linux/aowis-server-map --write-default-config ./map-server.ini
./build-linux/aowis-server-map --check-config --config ./map-server.ini
./build-linux/aowis-server-map --config ./map-server.ini
```

The write command refuses to overwrite an existing file unless explicitly given `--overwrite`. For all settings, CLI options, provider catalog ownership and geographic coverage rules, see [Configuration](docs/configuration.md).

## What the server provides

| Capability | Behavior |
| --- | --- |
| **Raster XYZ tiles** | Configurable OSM/CyclOSM/OpenTopoMap/ArcGIS providers, 256 MiB RAM cache, persistent disk cache, deduplicated upstream fetching |
| **Provider catalog** | Stable provider UUIDs, runtime GET/PUT editing, atomic persistence separate from the administrator INI |
| **Download controls** | Independent raster and terrain switches; optional named geographic regions and zoom-dependent coverage |
| **Normalized terrain** | 65 × 65 `.aowterrain` Web-Mercator tiles with versioned metadata and CRC32, on-demand at zooms 8–14 for Copernicus GLO-30; area-averaged below zoom 14, so coarse levels do not alias |
| **Elevation lookup** | WGS84 point sampling with explicit source and requested vertical-datum semantics |
| **DEM analysis windows** | Up to 512 × 512 native-source float32 samples in little-endian row-major order, for Hydrology and other consumers |
| **Cache administration** | Delete raster tiles, normalized terrain or corresponding native source cells; cancel upstream activity |
| **Concurrent requests** | Asynchronous HTTP replies, single-flight terrain jobs, bounded worker pools and memory reservations, disconnect-aware cancellation |

Copernicus GLO-30 is a **digital surface model (DSM)**, not a guaranteed bare-earth model; elevations use **EGM2008**, and cross-datum conversion is not silently performed. See [Terrain providers](docs/terrain-providers.md) and [Elevation contract](docs/terrain-elevation-contract.md).

## HTTP API at a glance

All endpoints are relative to the configured host and port. Raster provider IDs are **UUIDs**; `copernicus-glo30` is a **terrain dataset ID**, not a provider UUID.

| Method and path | Description |
| --- | --- |
| `GET /status` | Server status |
| `GET /map/v1/providers` | Raster provider catalog |
| `PUT /map/v1/providers` | Replace raster provider catalog (**no API-key check**) |
| `GET /<provider-uuid>/<z>/<x>/<y>.png` | Raster map tile |
| `GET /terrain/v1/datasets` | Terrain dataset catalog |
| `GET /terrain/v1/<dataset>/<z>/<x>/<y>.aowterrain` | Normalized terrain tile |
| `GET /terrain/v1/elevation?latitude=…&longitude=…` | Point elevation; optional `vertical_datum` |
| `GET /terrain/v1/analysis/grid?dataset=…&west=…&south=…&east=…&north=…&columns=…&rows=…` | Float32 DEM window |
| `GET /upstream/v1/activity` | Raster and terrain upstream activity |
| `DELETE /upstream/v1/map-tiles` | Cancel raster upstream downloads |
| `DELETE /upstream/v1/terrain` | Cancel terrain upstream downloads |
| `DELETE /cache/<uuid>/<z>/<xmin>/<xmax>/<ymin>/<ymax>` | Remove cached raster tiles |
| `DELETE /terrain/v1/cache/<dataset>/<z>/<xmin>/<xmax>/<ymin>/<ymax>` | Remove normalized terrain tiles |
| `DELETE /terrain/v1/source/<dataset>/<z>/<xmin>/<xmax>/<ymin>/<ymax>` | Remove corresponding native source cells |

GET endpoints use the **read key** when configured. DELETE endpoints require the separate **delete key**; an empty delete key disables these operations, rather than exposing them anonymously. Either key can be supplied in `X-API-Key` or as a Bearer token, according to the endpoint's role. A successful grid response uses `application/vnd.aowis.dem-f32le` with exactly `rows × columns × 4` bytes.

See the [HTTP API reference](docs/http-api.md) for concrete requests, content types, response codes, provider editing and deletion semantics.

## Data, caching and offline operation

The raster cache retains successfully cached imagery until explicitly deleted; there is **no automatic expiry or disk-size eviction**. Upstream responses marked `no-store` or `private` are not persisted. Changing a provider's upstream URL separates its disk-cache variant so old imagery is not silently reused.

Terrain has two persistent cache layers: original Copernicus Cloud Optimized GeoTIFFs and normalized `.aowterrain` heightfields. On a normalized miss, the server uses cached source COGs or downloads missing sources if allowed. An offline-only server can still normalize locally available source files. Interrupted downloads can resume under validated HTTP Range/ETag conditions; incomplete files are never published as complete COGs.

Geographic restrictions apply to **new downloads**, not to reads from local caches. A requested terrain tile may require an entire native 1° × 1° Copernicus source cell. There is no automatic regional prefetch, arbitrary GIS-file import or offline-package mounting.

For cache layout, limits, locking, cancellation and memory behavior see [Architecture and resource limits](docs/architecture.md). Provider licensing and attribution obligations remain the deployer's responsibility.

## Build and qualification

```bash
./compile_linux_tests.sh --debug       # build server + native tests, run Debug tests
./compile_linux_tests.sh --release     # build server + native tests, run Release tests
./compile_linux_tests.sh --both

./tools/qualify_release.sh             # Release suite, configuration smoke and lifecycle stress
./tools/qualify_release.sh --sanitizers # ASan/UBSan build and regression tests
./tools/qualify_release.sh --all       # both qualification modes
```

`qualify_release.sh` does **not** start a server listener. It keeps diagnostic logs in `build-linux-qualification/`; `--quick` performs a shorter smoke test. The test suite exercises caching, terrain formats, elevation/datum semantics, native downloads, admission and disconnect/shutdown races. Sanitizer qualification instruments this project's binaries, not the internals of uninstrumented Qt or libtiff packages.

See [Architecture and resource limits](docs/architecture.md#verification-and-benchmarking) for repeatable lifecycle stress and Linux RSS/CPU/I/O benchmark commands. Benchmarks should use known-good, locally cached data, not load-test public upstream services.

## Deployment

For a Debian package, build with a disposable Debian 13 Docker toolchain and optionally qualify the installed package in a second clean container:

```bash
./compile_deb_docker.sh          # Build the .deb into dist/
./compile_deb_docker.sh --test   # Also test install, HTTP, upgrade, removal and purge
```

The native `./compile_deb.sh --install-dependencies` remains available on Debian without Docker. See [Debian package building and qualification](docs/debian-package-build.md) for test scope and installation details. Install the resulting artifact on the target system with `sudo apt install ./dist/<package-filename>.deb`.

The package installs a dedicated `aowis-server-map` systemd service, preserves existing administrator settings and creates independent random API keys on initial configuration. The service uses an explicit `/etc/aowis-server-map/map-server.ini`, state under `/var/lib/aowis-server-map/` and disposable caches under `/var/cache/aowis-server-map/`.

- [systemd and Debian installation](configs/systemd/README.md) — paths, ownership, service administration and manual installation.
- [Apache reverse proxy](configs/apache/README.md) — local proxy or HTTPS with Certbot, headers and trusted-client restrictions.

**Do not publish the Qt HTTP listener directly to the Internet.** CORS does not authorize writes; the unauthenticated catalog PUT route needs a genuine network/proxy access restriction.

## Documentation

| Reference | Use it for |
| --- | --- |
| [Debian package build](docs/debian-package-build.md) | Docker builds, isolated installation and upgrade qualification |
| [Configuration](docs/configuration.md) | INI, CLI, providers, region/zoom policies, state-file precedence |
| [HTTP API](docs/http-api.md) | Requests, responses, authentication, cache and upstream operations |
| [Architecture and resource limits](docs/architecture.md) | Execution model, concurrency, memory, cancellation, test and benchmark tools |
| [Terrain sources](docs/terrain-providers.md) | Copernicus GLO-30, cache fill, resumable download, data provenance |
| [Elevation and DEM contract](docs/terrain-elevation-contract.md) | Coordinate/datum rules, sampling, error statuses and analysis data |
| [`.aowterrain` binary specification](docs/terrain-tile-format.md) | Exact v1 structure, quantization, CRS, CRC and compatibility |
| [systemd](configs/systemd/README.md) · [Apache](configs/apache/README.md) | Linux deployment |

The C++ `Aowis::Map::TerrainData` API is also used in-process by AOWIS Server Standalone/Hydrology integrations, avoiding an unnecessary localhost HTTP hop or duplicate Copernicus source cache. Hydrology owns its analysis outputs; Map owns terrain sources and retrieval policy.
