# Terrain providers

`TerrainData` separates provider-native source data from the normalized AOWIS terrain store.
Consumers never decode provider formats directly:

```text
remote/local provider source
        ↓
terrain provider
        ↓
TerrainTile (65×65)
        ↓
.aowterrain normalized cache
        ↓
renderer / point sampling
```

`TerrainProvider` is the provider boundary. A provider declares whether it supports a dataset and
normalized XYZ address, fetches/opens its native source material, and returns a provider-neutral
`TerrainTile`. `TerrainData` owns read-through cache policy and persists the encoded normalized tile.

## Copernicus GLO-30

The supported Internet-backed provider is `copernicus`, with dataset identifier:

```text
copernicus-glo30
```

It uses the public anonymous AWS Copernicus DEM GLO-30 Cloud Optimized GeoTIFF distribution:

```text
https://copernicus-dem-30m.s3.amazonaws.com
```

No Copernicus account, AWS account or API token is required for this source. Provider-native files
are cached under:

```text
terrain/providers/copernicus/glo30/
```

and normalized tiles continue to use the normal cache path:

```text
terrain/normalized/v1/copernicus-glo30/<z>/<x>/<y>.aowterrain
```

The provider records:

- nominal source resolution: 30 m;
- vertical datum: EGM2008 / EPSG:3855;
- dataset: `copernicus-glo30`.

Copernicus DEM is a **digital surface model (DSM)**. Heights may therefore represent vegetation,
buildings and infrastructure as well as bare ground. AOWIS must not present these values as surveyed
hydraulic elevations or a guaranteed bare-earth DTM.

GLO-30 Public is not complete in a small number of areas/countries. The provider does not silently substitute GLO-90: retaining the
`copernicus-glo30` dataset identifier would misrepresent the resolution and
source metadata. GLO-90 is not a configured dataset in this implementation.

### Slow connections, cancellation and offline operation

Provider-native source files are published only after successful complete
acquisition. Copernicus downloads use separately configurable connection,
inactivity and absolute-duration limits in `[terrain]`: 30 seconds, 120 seconds
and 1800 seconds respectively by default. Slow but progressing connections
can continue within the total deadline. The source-file lock wait checks for
cancellation so shutdown does not have to wait for the entire lock timeout.
The cross-process source lock stale threshold is at least ten minutes and
extends beyond the configured download deadline by one minute, preventing a
legitimate slow download from being mistaken for an abandoned cache fill.

Interrupted full downloads with a strong ETag and a known total size can retain
verified partial data. On the next attempt, the downloader sends `Range` and
`If-Range`. It accepts resumed bytes only when the returned `206 Content-Range`,
strong ETag and exact byte counts match; otherwise the attempt fails safely.
A server that ignores `Range` and responds with a complete `200 OK` is handled
as a fresh full download. The completed file is published atomically; partial
files are never used as terrain source cache entries. Transfers without a
usable validator may still need to restart from byte zero.

Native COGs already in cache, and normalized tiles already generated,
remain usable when Internet access is unavailable or remote fetching is
disabled. Upstream acquisition still runs within terrain worker operations;
it is not yet isolated in an independent network worker pool. For concurrency
budgets and remaining limitations see
[Performance, concurrency and operations](performance-roadmap.md).

### Dataset catalog

`CopernicusTerrainProvider::datasets()` describes the dataset for `GET /terrain/v1/datasets`:
nominal resolution 30 m, vertical datum `egm2008`, surface model `dsm`, normalized zooms 8 through
14. Every provider describes its datasets this way, so a renderer derives its maximum terrain zoom
from the catalog and a provider with another resolution needs no renderer change.

### Normalized zoom range

Remote GLO-30 generation currently supports normalized XYZ zooms 8 through 14.

- Zoom 14 gives a 65×65 normalized grid at approximately source-scale horizontal spacing.
- Higher zooms would mostly oversample the same 30 m source data.
- Zooms below 8 could make one normalized tile require an excessive number of 1° source COG files.

Cached normalized tiles outside this range remain valid and usable; the restriction applies only to
on-demand generation by this provider.

### Read-through behavior

Terrain uses a two-layer persistent read-through cache:

```text
normalized .aowterrain cache
        ↓ miss/corrupt
provider-native Copernicus COG cache
        ↓ miss
Internet source (only when remote fetching is enabled)
```

A supported `copernicus-glo30` request does the following:

1. validates and reads the normalized `.aowterrain` tile when it is already cached;
2. on a miss or corrupt normalized tile, acquires a lock for that exact dataset/XYZ tile and
   rechecks the cache so concurrent duplicate requests collapse onto one fill;
3. opens cached provider-native 1° COG files where available;
4. if a required COG is missing and remote fetching is enabled, acquires a lock for that source COG,
   rechecks the provider cache, then downloads it from the public AWS bucket;
5. bilinearly resamples the source raster into the normalized 65×65 Web-Mercator grid;
6. encodes and atomically writes the `.aowterrain` tile;
7. returns the same encoded bytes to the caller.

Different normalized tiles can be generated concurrently. Different Copernicus source COGs can also
download concurrently, while requests that need the same normalized tile or the same 1° COG are
deduplicated by their cache-fill locks. Lock files also protect multiple map-server processes sharing
one cache directory.

A corrupt normalized tile is never served. The read-through path attempts to regenerate it from the
provider-native cache or remote provider; if repair is impossible, the original corruption remains a
reported integrity error.

Subsequent renderer or point-elevation requests use the normalized cache and require no network.
When `terrain/remote_fetch_enabled=false`, providers may still normalize data that already exists in
their provider-native cache, but they must not perform any network request.

Provider-native GeoTIFF decoding uses libtiff. The COG's DEFLATE compression, floating-point
predictor and byte ordering are handled by libtiff; AOWIS reads only decoded raster blocks needed for
the normalized tile after the source COG has been cached locally.

The HTTP terrain route runs read-through generation on QtConcurrent rather than blocking the map
server's main event loop. Cache-fill locking deduplicates matching requests without globally
serializing unrelated terrain generation.

### Interrupted download recovery

A failed HTTP 200 transfer can retain its downloaded prefix in a separate
`.partial` file, accompanied by atomic `.partial.meta` metadata containing the
strong ETag, total size and exact saved byte count. A mismatched or corrupt
metadata pair is discarded, and the source is downloaded from the beginning.

A later request issues `Range: bytes=<offset>-` with `If-Range: <ETag>`.
A valid `206 Partial Content` must begin at the requested offset, end at the
recorded total length, and carry the same strong ETag. The validated prefix
and suffix are assembled into the final `.tif` using atomic `QSaveFile`
publication. A complete `200 OK` response is accepted as a new full entity
when the provider ignores the range request.

Resumption is best-effort. If the provider omits a strong ETag or total length,
failed transfers are not retained. Range responses with inconsistent metadata
are rejected rather than risk corrupting a terrain source. There is currently
no independently scheduled upstream I/O pool or automatic background retry.

## Licensing and attribution

Copernicus GLO-30 Public is distributed for public use under the Copernicus DEM licence. Any externally assembled offline package must retain appropriate dataset
provenance and attribution. The normalized
binary tile itself intentionally does not embed licensing text; attribution belongs to package/server
metadata, not each 8 KiB heightfield payload.

### Administrative configuration and geographic download policies

The server has separate `downloads/remote_fetch_enabled` (2D maps) and
`terrain/remote_fetch_enabled` (terrain) switches. The Copernicus implementation
is selected with `[terrain_provider.copernicus]` (`enabled=true/false`, optional
`coverage=`). When the terrain subsystem's upstream switch is off, the provider
cannot override it. When a normalized terrain tile is not in the cache, the
terrain provider may still normalize existing locally cached native source data.
Only acquisition of missing Internet source data is blocked. The server does
not hide cached data based on the requested geographic region.

Named regions and zoom-dependent profiles are documented in the root README.
A profile's geographic tile intersection is evaluated before the provider is
allowed to acquire missing source data. Note that one Copernicus COG covers a
whole degree cell, which may extend beyond the smaller allowed XYZ tile footprint;
the policy governs the requested normalized tile, not clipping of the upstream COG.

Repeated interruptions during validated HTTP 206 resumes retain newly received bytes when the response range, strong ETag and declared length match the saved entity. Partial files remain unpublished until complete.

### Local HTTP resume regression

The `aowis-server-map-copernicus-resume-http` CTest case uses a loopback HTTP
server to exercise the production source downloader without external internet.
It interrupts a full response and a subsequent range response, verifies
`Range`/`If-Range`, and checks that only the completed concatenated source is
published. It also verifies full-response fallback when a server ignores Range,
a changed upstream ETag, and rejection of a mismatched 206 response. The source
URL override is an internal test seam; production Copernicus URLs remain fixed.

The local Copernicus HTTP integration test also exercises slow-but-progressing transfers
and a stalled peer with shortened test-only timeouts. Production timeout defaults
and the upstream URL are unchanged.

The local slow-network regression suite independently checks that ongoing progress avoids
inactivity timeout, that stalled transfers terminate on inactivity, and that an
absolute deadline still stops a continuously progressing but overlong transfer.
These checks use short test-only limits and do not alter production defaults.
