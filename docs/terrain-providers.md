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
  .aowterrain v2 cache
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

On-demand normalization stops at zoom 14 because higher zooms would mostly oversample the underlying source, and below zoom 8 a normalized tile can span too many native source cells. Existing valid normalized tiles outside this generation range can still be read. The optional visual-only global overview supplies zoom 0–7; see below.

## Cache organization and read-through behavior

```text
<terrain cache>/
  providers/copernicus/glo30/         Native downloaded source COGs
  normalized/v2/r2/copernicus-glo30/ <z>/<x>/<y>.aowterrain
  offline-packages/                  Optional global-overview-v1.aowpack
```

On a normalized tile request the server:

1. Validates the cache entry's format, embedded dataset/XYZ address and CRC; no invalid tile is delivered.
2. For a miss or corrupt entry, obtains the normalized tile's cache-fill lock and rechecks it.
3. Opens already cached native COGs covering the requested tile.
4. If native data is missing and remote fetching is enabled/allowed, obtains a per-source interprocess lock and downloads the missing COG.
5. Uses libtiff to decode only needed raster tiles or strips, normalizes them to the common 65 × 65 heightfield (see below), and atomically writes the `.aowterrain` bytes.
6. Returns the validated normalized bytes to the caller.

Cross-process cache-file locks deduplicate source acquisition even when two server instances share a source cache. The server also single-flights identical in-process terrain HTTP requests. Different tiles and independent native source cells can proceed concurrently subject to admission and worker limits. A damaged normalized file is repaired from locally available native sources; when they are missing, the integrity/read error is reported rather than serving corrupt data. Deleting the damaged normalized tile (`DELETE /terrain/v1/cache/…`) makes the next request acquire its sources again.

Normalized tile format or normalization revision changes must not change the native
Copernicus cache namespace. Existing readable, nonempty native COGs are reused
without contacting Copernicus, including when remote acquisition is enabled.
Optional LOD metadata scans must not acquire additional source files. A cold
normalized cache may require local regeneration or MAP-to-MAP transfer, not
a second third-party download of a retained source. New geographic coverage
can still require source files that were never cached.


The samplers assume the GLO-30 grid: *RasterPixelIsPoint*, the centre of pixel (0, 0) on the cell's north-west corner, exactly `width × height` pixels per degree (columns narrow above ±50° latitude, rows are always 1″). A native file that carries GeoTIFF georeferencing (`ModelTiepointTag`, `ModelPixelScaleTag`, `GTRasterTypeGeoKey`) is checked against that grid when opened and rejected as a source read error if it disagrees, so a reprocessed upstream product fails loudly instead of shifting elevations. Files without georeferencing tags are accepted.

### How normalized samples are derived

At **zoom 14**, the native zoom, the sample spacing is about the 30 m source spacing; every sample is the **bilinear** interpolation of the four surrounding source pixels.

At **zooms 8–13** a sample stands for much more ground than a pixel (about 2.4 km at zoom 8 at the equator, 80 × 80 source pixels). A point sample there aliases: ridges, peaks and valleys between samples are lost or land at random, and hillshading makes that visible. Each sample is therefore the **area average** of the source over its sample box:

- the box is centred on the sample and spans half a sample spacing to each side in XYZ (Web-Mercator) space, so the boxes of one zoom tile the plane and adjacent tiles compute identical values for their shared edge samples;
- every source pixel footprint (centre ± half a pixel, *RasterPixelIsPoint*) contributes with its overlap area with the box, in square degrees; no-data pixels and missing source cells contribute nothing, and a box with no valid source is no-data;
- boxes of the westernmost and easternmost tiles reach across ±180° and read the source cells on the other side;
- where a box is smaller than one source pixel (zoom 13 at high latitudes, where GLO-30 columns widen and Mercator rows shrink), the sample stays bilinear, which is the better estimate there. The choice depends only on the sample, so shared edges still agree.

Each source block is decoded once per tile however many boxes read it; a zoom-8 tile costs about 20 % more than with point samples, dominated by decoding the same blocks. The source cells acquired for a tile include the half spacing its boxes reach beyond it.

The normalization revision (`TerrainNormalizationRevision`, currently 2) is part of the cache path. Tiles cached in the obsolete `normalized/v1/` format namespace are never read again; that directory can be deleted with MAP stopped. Native provider data is reused.

### Source elevation statistics (LOD groundwork)

Fresh local normalization also returns optional `TerrainSourceStatistics` in the
C++ `TerrainProviderFetchResult` and `TerrainTileLookupResult`. This records
minimum/maximum **native** valid elevation, valid and no-data pixel counts, and
unavailable candidate source cell count. Each visited native pixel is counted
once, not once per overlapping sample box. At zooms 8–13 the existing area
average scan includes the half-sample halo; at zoom 14 a separate scan covers
the tile footprint, reusing decoded blocks and without initiating new remote
source downloads. Native zoom statistics are omitted if that optional scan fails.

The observations are in the dataset's native vertical datum. They are not yet
certified conservative culling bounds: missing sources/no-data, interpolation
support and datum/mesh transformations must be accounted for before using them
as bounds. A missing candidate count may include a cell with no actual overlap.
An isolated peak can be represented by these statistics even when lost from the
averaged 65 × 65 grid.

The optional statistics are persisted in the v2 tile payload and preserved through cache hits and cascading MAP servers. Missing statistics remain unknown; they are not fabricated from averaged samples. Normalization revision and height values are unchanged.

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

Coverage rules govern **requested XYZ tiles that may trigger new downloads**. A native 1° source file can extend beyond that request footprint; it is not clipped geographically. Already cached native files can still normalize tiles outside download coverage. Configuring a `permanent_data_directory` does not automatically mount arbitrary terrain packages or import GIS datasets; the explicitly named global overview is the exception. See [Configuration](configuration.md#geographic-download-coverage).

## Vertical datums, provenance and attribution

Copernicus values are EGM2008-referenced. AOWIS records source datum metadata in normalized tiles and point-elevation responses and does **not** silently perform EGM2008 ↔ EGM96 ↔ WGS84-ellipsoid transformations. A datum requiring a real conversion is rejected. See [Terrain and elevation contract](terrain-elevation-contract.md).

Copernicus GLO-30 Public is distributed under the Copernicus DEM licence. Respect the provider's terms and preserve attribution/provenance for externally packaged datasets. The `.aowterrain` binary format intentionally excludes licensing text; attribution must be maintained by surrounding dataset, package or application metadata.

## Local terrain approximation diagnostics

Fresh Copernicus normalization returns optional native source statistics and
`TerrainGeometricError` in the provider and tile lookup results. The error is
measured by a second local, block-wise scan of native pixel **centres inside the
XYZ tile**, excluding the averaging halo. It compares the source height with
piecewise-linear interpolation of the unquantized 65 × 65 grid in Web Mercator
coordinates, using the NE–SW diagonal of Render's globe surface index buffer.
It records maximum positive and negative residual magnitudes, unweighted RMS
and counts of compared centres, source no-data centres, centres without a
finite triangle and unavailable candidate source cells. Missing data is not a
zero-height sample. No compared centres or a scan failure produces absent
error metadata, not zero error. The extra scan cannot download sources or
invalidate a successfully normalized tile; it uses bounded raster block
caching and per-axis coordinate arrays rather than a full native raster copy.
It does add local CPU and possibly raster decode work for newly generated tiles.

These values are observed vertical errors, not conservative continuous or
ECEF bounds; they exclude quantization, ellipsoid curvature, polar edge
flattening, vertical exaggeration, skirts, hole filling and ancestor fallback.
The v2 tile payload carries both optional records, including through HTTP and cascades. Existing v1 normalized tiles are not used. Analysis sampling and rendering behavior are unchanged.


## Bundled global visual overview (zoom 0–7)

Coarse globe tiles must not initiate huge Copernicus downloads. Standard MAP
Debian packages and development builds now include a precomputed, immutable
`global-overview-v1.aowpack` derived from ETOPO. See [global terrain data](global-terrain-data.md).
An administrator can override this pack with one in the offline package directory (`<terrain cache>/offline-packages/`, or
`<permanent_data_directory>/terrain/` if configured). The pack contains 21,845
canonical v2 tiles, indexed by zoom, row and column (about 188 MB). It is not
part of the normalized LRU cache and no native source files are invalidated.
The catalog advertises `minimum_zoom: 0` and `native_minimum_zoom: 8` only
when the pack header and size are valid. Each tile's CRC, address and datum
are independently validated on read. Install the pack before starting clients
(or reconnect to refresh the catalog).

Build once on a machine with Python, NumPy and Rasterio/GDAL:

```bash
python3 tools/build_global_terrain_overview.py /path/to/global-land-egm2008.tif \
    /path/to/global-overview-v1.aowpack --source-resolution-m 1855
```

Input must be a north-up global EPSG:4326 GeoTIFF with finite elevations in
metres relative to EGM2008, with the ocean filled at sea level. One possible
input is NOAA ETOPO 2022 60 arc-second ice-surface relief
(https://www.ncei.noaa.gov/products/etopo-global-relief-model). ETOPO includes
bathymetry: prepare a land-only raster with the ocean set to zero, or explicitly
pass `--clamp-negative` for a visual-only approximation that also removes
below-sea-level land. The source resolution argument is the approximate input
resolution, not the output grid spacing. Keep the input for future builds.
The tool does not download anything. It computes area means in Web Mercator,
then builds coarser levels with 2x2 averages and samples shared vertex edges.
The output is atomically published, and can be copied to multiple MAP servers.
Install it with read permission for the MAP service user (for example,
`install -m 0644 global-overview-v1.aowpack <offline package directory>/`).
Preserve the source attribution and source file alongside your provisioning records.
This is an overview derived from a separate source, not native GLO-30. Native
source-statistics and geometric-error fields are intentionally absent in these
coarse tiles. The native GLO-30 generation range and existing caches are unchanged.
Point elevation queries for GLO-30 do not fall back to these visual-only levels;
analysis-grid APIs are unchanged. Polar areas beyond Web Mercator are not covered.

Every standard MAP installation includes its own local overview pack. The ordinary cascade protocol can also transfer individual
v2 coarse tiles on explicit requests, but automatic upstream overview discovery
is not implemented. This avoids advertising coverage from an upstream that may
not have the pack. The low-level overview builder and runtime serving never fetch third-party data; the separate provisioning tool acquires ETOPO once on the build host if no original is available.
