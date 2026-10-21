# Terrain and elevation contract

AOWIS Server Map provides one terrain/elevation subsystem for point sampling
and normalized terrain heightfields. This document defines the existing HTTP
and C++ sampling contract. Both uses share the same cache and source-dataset
metadata; clients need not implement provider-specific elevation lookups.

See [terrain providers](terrain-providers.md) for supported data sources and
[the normalized format](terrain-tile-format.md) for `.aowterrain` encoding.

## Coordinate contract

Point elevation requests use WGS84 geographic coordinates:

- latitude: degrees, inclusive range `[-90, 90]`;
- longitude: degrees, inclusive range `[-180, 180]`.

Invalid or non-finite coordinates must be rejected before provider/cache access.

## Elevation sample contract

`Aowis::Map::TerrainElevationSample` is the canonical in-process result type.

Every successful sample contains:

- `elevation_m`: terrain elevation in metres;
- `dataset`: stable provider/dataset identifier, for example `copernicus-glo30`;
- `nominal_resolution_m`: nominal source resolution in metres, or `0` only when genuinely unknown;
- `vertical_datum`: datum of the returned numeric elevation;
- `source_vertical_datum`: datum carried by the normalized source tile before any transformation;
- `origin`: where this answer was obtained: cache, remote source, offline package or local dataset.

The source datum and returned datum are intentionally separate fields. They are equal for native/identity sampling. A future geoid transformation may make them differ, but a known returned datum may never be claimed from an unknown source datum.

A result is invalid when the elevation is not finite, the resolution is negative/non-finite, or the dataset identifier is empty.

## Stable identifier values

Vertical datum identifiers:

- `unknown`
- `wgs84-ellipsoid` — ellipsoidal height carried by WGS 84 3D (`EPSG:4979`);
- `egm96` — orthometric/gravity-related EGM96 height (`EPSG:5773`);
- `egm2008` — orthometric/gravity-related EGM2008 height (`EPSG:3855`);
- `local` — an explicitly local/project vertical reference with no globally implied CRS.

`local` deliberately does **not** mean that two different local datasets share the same datum. Format v1 carries only the `local` class, not a globally unique local-datum identifier, so local heights must not be converted or compared across datasets without separate project metadata.

Data-origin identifiers:

- `unknown`
- `cache`
- `remote`
- `offline-package`
- `local-dataset`

These string identifiers are part of the HTTP contract and must remain stable. New values may be added later without changing existing meanings.

## Cached point lookup statuses

`TerrainData::sampleElevation()` returns a structured lookup result rather than a naked optional value. Stable status identifiers are:

- `ready`
- `disabled`
- `not-initialized`
- `invalid-coordinate`
- `invalid-dataset`
- `invalid-vertical-datum`
- `vertical-datum-conversion-unavailable`
- `outside-coverage`
- `tile-unavailable`
- `tile-read-error`
- `corrupt-tile`
- `no-data`

This distinction lets the server report invalid requests, unavailable data,
remote-fetch problems and cache-integrity failures separately.

## Point-elevation HTTP representation

The map server exposes the canonical point sampler as:

```text
GET /terrain/v1/elevation?latitude=<degrees>&longitude=<degrees>[&vertical_datum=<datum>]
```

`vertical_datum` is optional. Omitted, empty, or `native` returns the dataset's native datum. Explicit output requests currently accept `wgs84-ellipsoid`, `egm96`, or `egm2008`. `unknown` and `local` cannot be requested as output datums.

The map server chooses the dataset from `terrain/default_dataset` (default `copernicus-glo30`), so GUI consumers do not hardcode a provider. The route uses the same read API-key authentication as raster and terrain-tile GET requests. Provider/cache work runs outside the main HTTP event loop. A successful response serializes the canonical result without provider-specific response shapes:

```json
{
  "status": "ready",
  "elevation_m": 232.4,
  "dataset": "copernicus-glo30",
  "nominal_resolution_m": 30.0,
  "vertical_datum": "egm2008",
  "vertical_datum_name": "EGM2008 geoid height",
  "vertical_datum_authority": "EPSG:3855",
  "vertical_reference": "orthometric",
  "source_vertical_datum": "egm2008",
  "origin": "cache",
  "tile": {
    "zoom": 14,
    "x": 8586,
    "y": 5573
  }
}
```

`tile` identifies the normalized heightfield actually sampled. Error responses use the stable terrain lookup status ID plus an `error` string. Datum errors additionally include `requested_vertical_datum` and, when source data were reached, `source_vertical_datum`. HTTP status mapping is intentionally conventional: malformed coordinates, invalid configured datasets, invalid datum requests and unavailable datum transformations use `400`; unavailable/out-of-coverage/no-data use `404`; disabled/uninitialized terrain uses `503`; upstream network failures use `502`; corrupt/provider/internal failures use `500`.

## Offline/online rule

The same normalized terrain store must serve both point sampling and 3D terrain tiles.

A point lookup must therefore follow the same availability rules as terrain rendering:

1. use normalized local/cache/offline data when present;
2. if data is absent and remote fetching is enabled, fetch and normalize provider data;
3. if data is absent and remote fetching is disabled/unavailable, report terrain data as unavailable.

Clients using this API receive the server's configured elevation dataset and
its download policy. A client that instead calls a third-party elevation API
is not subject to this server's restrictions.

## Vertical datum rule

AOWIS must not silently treat terrain values from unknown/different vertical datums as interchangeable with hydraulic/project elevations. Providers must write their actual source datum into every normalized tile; if it is genuinely unknown, the value stays `unknown`.

Point sampling may request an explicit output datum. At present AOWIS intentionally implements **identity handling only**: a request for the same globally defined datum succeeds, while a request that would require EGM96 ↔ EGM2008 ↔ WGS84 ellipsoid transformation returns `vertical-datum-conversion-unavailable`. AOWIS does not apply a guessed constant offset, and it does not pretend that a datum-name change is a coordinate transformation.

A real cross-datum conversion requires a geoid/vertical transformation model evaluated at the requested horizontal coordinate. That belongs behind the terrain subsystem as a later transformation provider. Until such a model is configured, callers must either consume the dataset's native datum or explicitly handle the mismatch.

The `local` datum class is intentionally not requestable as an output datum because v1 does not carry a unique local vertical-reference identifier. A future local-DEM/project-datum extension must provide that identity before transformations involving local heights are permitted.

## Terrain subsystem boundary

`Aowis::Map::TerrainData` is the dedicated terrain/elevation subsystem owned by `AOWIS-SERVER-MAP`. It is a sibling of the raster `MapTiles` subsystem; terrain behavior must not be added to `MapTiles`.

The subsystem owns terrain lifecycle and storage policy:

- terrain can be enabled or disabled independently of raster map tiles;
- remote fetching policy is recorded independently so an installation can operate in offline-only mode;
- a dedicated terrain cache root is resolved either from `terrain/cache_directory` or `<downloads cache>/terrain`;
- storage separates normalized AOWIS terrain, provider-native cached data and the offline-package directory.

The normalized terrain binary format is defined separately in [`terrain-tile-format.md`](terrain-tile-format.md). `TerrainData::sampleElevation()` samples those normalized cached tiles directly. It selects the finest cached tile containing the requested WGS84 coordinate, maps the point into the 65×65 heightfield and performs bilinear interpolation. If a contributing heightfield sample is no-data, sampling falls back to a coarser cached tile when one is available. Corrupt or unreadable normalized tiles are reported explicitly rather than silently skipped.

The sampler accepts an explicit dataset identifier. A normalized cache hit reports `TerrainDataOrigin::Cache`; a tile generated by the remote provider during the current request reports `TerrainDataOrigin::Remote`. Provider-native formats remain hidden behind `TerrainProvider`. Offline-package mounting is not implemented in the current retrieval path. The HTTP point-elevation endpoint uses this sampler.

## Shared tile retrieval path

Point sampling and renderer tile delivery use the same normalized terrain retrieval boundary. `TerrainData::sampleElevation()` resolves each candidate XYZ tile through `TerrainData::terrainTile()` before decoding/interpolating it.

The shared path performs read-through retrieval and cache-integrity handling,
so point elevation queries and 3D tile consumers share the same availability
and source behavior. Offline-package mounting is not implemented here.


## Supported remote dataset

`copernicus-glo30` is the Internet-backed terrain dataset currently supported. It is normalized from the public Copernicus GLO-30 COG distribution, records EGM2008 as its vertical datum and 30 m as nominal source resolution, and is a DSM rather than a guaranteed bare-earth DTM. See [`terrain-providers.md`](terrain-providers.md).
