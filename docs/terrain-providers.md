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

The first Internet provider is `copernicus` with dataset identifier:

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

GLO-30 Public is not complete in a small number of areas/countries. Version 1 of the provider does
not silently substitute GLO-90, because doing so while retaining the `copernicus-glo30` dataset ID
would make source/resolution metadata dishonest. A later provider-selection layer may explicitly
request `copernicus-glo90` as a separate dataset.

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

## Licensing and attribution

Copernicus GLO-30 Public is distributed for public use under the Copernicus DEM licence. Offline
package manifests must eventually retain dataset provenance and required attribution. The normalized
binary tile itself intentionally does not embed licensing text; attribution belongs to package/server
metadata, not each 8 KiB heightfield payload.
