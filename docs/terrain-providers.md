# Terrain sources and caching

`Aowis::Map::TerrainData` is the terrain/elevation subsystem. It separates provider-native raster files from AOWIS's validated normalized heightfields and exposes the same source/cache policy to renderer tiles, point sampling and analysis grids.

```text
Copernicus source COG (native GeoTIFF)
          |              |
          |              +--> analysisGrid(): source-pixel float32 window
          v
  TerrainProvider normalization
          v
  TerrainTile (65 × 65, float metres)
          v
  .aowterrain v1 cache
          |
          +--> renderer terrain tiles / point elevation
```

The `TerrainProvider` boundary reports supported datasets, acquires or opens native source files and produces provider-neutral `TerrainTile` objects. `TerrainData` controls read-through caching, integrity checks and persistence. Hydrology can consume `TerrainData::analysisGrid()` directly, avoiding separate provider caches or duplicate download implementations.

## Copernicus GLO-30

The supported Internet-backed terrain dataset is **`copernicus-glo30`**, served by the `copernicus` provider from the publicly accessible Cloud Optimized GeoTIFF (COG) distribution:

```text
https://copernicus-dem-30m.s3.amazonaws.com
```

The public source does not require an AWS account or API token. Dataset properties:

| Property | Value |
| --- | --- |
| Dataset identifier | `copernicus-glo30` |
| Nominal source resolution | 30 m |
| Surface classification | **Digital surface model (DSM)** |
| Vertical datum | EGM2008 (`EPSG:3855`) |
| Provider-native geography | 1° × 1° source COGs |
| Normalized XYZ generation | Zooms **8–14** |
| Normalized grid | 65 × 65 Web-Mercator samples |

A DSM may include building roofs and vegetation. It is **not** equivalent to a surveyed bare-earth digital terrain model or a hydraulic elevation reference. Public GLO-30 coverage is not universal; the server does not silently substitute GLO-90 and mislabel it as GLO-30. The authoritative dataset metadata is available from `GET /terrain/v1/datasets`.

On-demand normalization stops at zoom 14 because higher zooms would mostly oversample the underlying source, and below zoom 8 a normalized tile can span too many native source cells. Existing valid normalized tiles outside this generation range can still be read.

## Cache organization and read-through behavior

```text
<terrain cache>/
  providers/copernicus/glo30/         Native downloaded source COGs
  normalized/v1/copernicus-glo30/    <z>/<x>/<y>.aowterrain
  offline-packages/                  Reserved; no automatic mounting
```

On a normalized tile request the server:

1. Validates the cache entry's format, embedded dataset/XYZ address and CRC; no invalid tile is delivered.
2. For a miss or corrupt entry, obtains the normalized tile's cache-fill lock and rechecks it.
3. Opens already cached native COGs covering the requested tile.
4. If native data is missing and remote fetching is enabled/allowed, obtains a per-source interprocess lock and downloads the missing COG.
5. Uses libtiff to decode only needed raster tiles or strips, bilinearly normalizes samples to the common 65 × 65 heightfield, and atomically writes the `.aowterrain` bytes.
6. Returns the validated normalized bytes to the caller.

Cross-process cache-file locks deduplicate source acquisition even when two server instances share a source cache. The server also single-flights identical in-process terrain HTTP requests. Different tiles and independent native source cells can proceed concurrently subject to admission and worker limits. A damaged normalized file is repaired from locally available native sources; when they are missing, the integrity/read error is reported rather than serving corrupt data. Deleting the damaged normalized tile (`DELETE /terrain/v1/cache/…`) makes the next request acquire its sources again.

The samplers assume the GLO-30 grid: *RasterPixelIsPoint*, the centre of pixel (0, 0) on the cell's north-west corner, exactly `width × height` pixels per degree (columns narrow above ±50° latitude, rows are always 1″). A native file that carries GeoTIFF georeferencing (`ModelTiepointTag`, `ModelPixelScaleTag`, `GTRasterTypeGeoKey`) is checked against that grid when opened and rejected as a source read error if it disagrees, so a reprocessed upstream product fails loudly instead of shifting elevations. Files without georeferencing tags are accepted.

Original native COG files also serve **analysis grids** directly: output grid samples are selected from the containing source pixels, not bilinearly resampled from `.aowterrain`. This distinction matters for Hydrology and other numerical consumers. See the [elevation/analysis contract](terrain-elevation-contract.md) and [tile format](terrain-tile-format.md).

## Remote acquisition, slow networks and resumption

Connection, inactivity and absolute download deadlines are separately configurable under `[terrain]`:

```ini
[terrain]
connect_timeout_seconds=30
inactivity_timeout_seconds=120
download_deadline_seconds=1800
```

A slow but progressing response can continue within the absolute duration limit. The source download is bounded to **256 MiB**. Native source files are not published until the complete content has been validated and atomically committed. Interprocess source locks have a stale threshold beyond the configured download deadline to avoid stealing a slow but live acquisition. Lock waiting and active transfers check cancellation requests.

A failed full response may retain a verified prefix in `.partial` and an atomic `.partial.meta` sidecar containing a **strong ETag**, declared entity length and saved byte count. On the next attempt, a `Range: bytes=<offset>-` with `If-Range: <ETag>` requests the remainder. A `206` is accepted only when its `Content-Range`, ETag and length match the saved entity. A full `200` response from a server ignoring Range is handled as a fresh full download. Inconsistent metadata causes safe rejection, not publication of a corrupt source. If the upstream supplies no suitable validator/length, partial data cannot be trusted for resumption and the next attempt starts at byte zero.

The server performs no background retry or bulk download scheduling. Source acquisition uses remote-capable workers, followed by a CPU-pool handoff for normalization, analysis-grid sampling and point-elevation processing. This prevents slow network waits from occupying cached-processing workers. See [Architecture and resource limits](architecture.md) for shared budgets and cancellation semantics.

## Offline and geographic download policies

```ini
[terrain]
enabled=true
remote_fetch_enabled=false

[terrain_provider.copernicus]
enabled=true
```

`terrain/enabled=false` makes terrain requests unavailable (`503`); `terrain/remote_fetch_enabled=false` allows the routes and **local source/normalized caches**, but forbids missing Internet source downloads. This is separate from `downloads/remote_fetch_enabled`, which controls raster imagery. The Copernicus provider may also be disabled or assigned a named geographic coverage profile, but it cannot override the global switch.

Coverage rules govern **requested XYZ tiles that may trigger new downloads**. A native 1° source file can extend beyond that request footprint; it is not clipped geographically. Already cached native files can still normalize tiles outside download coverage. Configuring a `permanent_data_directory` does not automatically mount arbitrary terrain packages or import GIS datasets. See [Configuration](configuration.md#geographic-download-coverage).

## Vertical datums, provenance and attribution

Copernicus values are EGM2008-referenced. AOWIS records source datum metadata in normalized tiles and point-elevation responses and does **not** silently perform EGM2008 ↔ EGM96 ↔ WGS84-ellipsoid transformations. A datum requiring a real conversion is rejected. See [Terrain and elevation contract](terrain-elevation-contract.md).

Copernicus GLO-30 Public is distributed under the Copernicus DEM licence. Respect the provider's terms and preserve attribution/provenance for externally packaged datasets. The `.aowterrain` binary format intentionally excludes licensing text; attribution must be maintained by surrounding dataset, package or application metadata.
