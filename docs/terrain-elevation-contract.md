# Terrain elevation and analysis-grid contract

AOWIS Server Map exposes the same terrain dataset through three complementary interfaces:

- **Normalized tile delivery:** 65 × 65 Web-Mercator heightfields for rendering and point sampling.
- **Point elevation:** one WGS84 coordinate, sampled through the normalized tile store with datum and provenance metadata.
- **Analysis grid:** a rectangular float32 window sampled from the **native source GeoTIFF**, not from quantized renderer tiles.

This document defines data meanings and stable identifiers for HTTP and `Aowis::Map::TerrainData` callers. For request URLs and HTTP status codes see the [API reference](http-api.md); for the source see [Copernicus terrain](terrain-providers.md).

## Coordinates and units

Point queries use WGS84 geographic coordinates in decimal degrees: `latitude` in `[-90,90]`, `longitude` in `[-180,180]`. Invalid/non-finite values are rejected before provider access. Available normalized Web-Mercator tiles cover the usual ±85.05112878° latitude range, so a syntactically valid polar point can still lack terrain data.

Elevations are **metres** in the dataset's stated vertical datum. Copernicus GLO-30 is a **digital surface model** (DSM) in EGM2008, so its elevations may include vegetation and structures; they are not necessarily bare-earth or surveyed hydraulic heights.

## Point sample representation

`Aowis::Map::TerrainElevationSample` is the canonical C++ result; the point HTTP response carries the corresponding JSON fields:

| Field | Meaning |
| --- | --- |
| `status` | Lookup status, `ready` on success |
| `elevation_m` | Finite elevation in metres |
| `dataset` | Stable source dataset ID, e.g. `copernicus-glo30` |
| `nominal_resolution_m` | Source's nominal horizontal resolution, `0` only if unknown |
| `vertical_datum` | Datum of the returned numeric value |
| `source_vertical_datum` | Native datum recorded in the normalized source tile |
| `origin` | How this answer was obtained |
| `tile` | Normalized XYZ tile sampled, with `zoom`, `x` and `y` |

A successful response also describes the vertical datum with fields such as `vertical_datum_name`, `vertical_datum_authority` and `vertical_reference`.

Example **shape** (the elevation and address below are illustrative, not a guaranteed measurement):

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
  "tile": { "zoom": 14, "x": 8586, "y": 5573 }
}
```

Source and returned datums are separate fields so a caller cannot confuse a label with an actual vertical transformation. At present all successful conversions are identity conversions: the source and output datums match.

### Stable identifiers

| Vertical datum ID | Meaning |
| --- | --- |
| `unknown` | Datum cannot be established |
| `wgs84-ellipsoid` | Ellipsoidal height under WGS 84 3D (`EPSG:4979`) |
| `egm96` | Gravity-related/orthometric height (`EPSG:5773`) |
| `egm2008` | Gravity-related/orthometric height (`EPSG:3855`) |
| `local` | A project-specific datum with no globally implied reference |

`local` is a *class*, not an identifier proving that two local datasets share the same vertical reference. V1 `.aowterrain` does not contain a globally unique local-datum identity. Do not mix local elevations between projects without external reference metadata.

Origin IDs: `unknown`, `cache`, `remote`, `offline-package`, `local-dataset`. These describe provenance, **not** different numeric datums. The origin enum includes offline-package/local-dataset values even though automatic offline-package mounting is not implemented.

Lookup status identifiers: `ready`, `disabled`, `not-initialized`, `invalid-coordinate`, `invalid-dataset`, `invalid-vertical-datum`, `vertical-datum-conversion-unavailable`, `outside-coverage`, `tile-unavailable`, `tile-read-error`, `corrupt-tile`, `no-data`. Clients should handle errors by `status`, not only by a human-readable `error` message.

## Sampling behavior and availability

`TerrainData::sampleElevation()` resolves point queries through `TerrainData::terrainTile()`. It selects the finest available normalized tile containing the point, converts the coordinate into a position within the 65 × 65 grid, and **bilinearly interpolates**. Normally that is a zoom-14 tile of bilinear source samples; a coarser tile holds area averages (zooms 8–13, see [Terrain providers](terrain-providers.md)), which are smoother than the point elevation. If contributing samples are no-data, the sampler can try a coarser cached normalized tile. Corrupt or unreadable cache entries are reported, not silently treated as valid elevations.

When remote acquisition is allowed, the HTTP path acquires native sources needed for the finest supported tile on the remote-capable pool and then hands CPU-only sampling to the terrain processing pool. Remote admission is released at the handoff; global job admission lasts until the computation ends. Coarser cached tiles may be used for fallback without initiating extra remote downloads from the CPU workers. In offline mode, cached normalized or native source data is still usable, but missing Internet sources are not fetched.

`GET /terrain/v1/elevation?latitude=50.1&longitude=8.1` uses `terrain/default_dataset` (default `copernicus-glo30`). A `vertical_datum` query value of omitted/empty/`native` returns native height. Explicit requests support `wgs84-ellipsoid`, `egm96` or `egm2008`; `local` and `unknown` are not requestable output datums.

**No cross-datum conversion model is installed.** An EGM2008 sample requested as WGS84 ellipsoid or EGM96 returns `vertical-datum-conversion-unavailable`, normally HTTP `400`, rather than an approximate constant offset or relabeling. Conversions require a spatially evaluated geoid/transformation model; callers must use compatible datums themselves until one is available.

HTTP error mapping for point lookups: bad parameters, invalid datasets, invalid datum and unavailable conversions → `400`; outside coverage/unavailable/no-data → `404`; upstream acquisition failures → `502`; disabled/uninitialized terrain → `503`; corrupt/provider/internal errors → `500`. Datum errors include `requested_vertical_datum` and, where known, `source_vertical_datum`.

## Analysis-grade DEM windows

The analysis API is intended for numerical consumers such as Hydrology that need **original source elevations**, not the quantized normalized renderer format.

```text
GET /terrain/v1/analysis/grid?dataset=copernicus-glo30&west=8.10&south=50.10&east=8.11&north=50.11&columns=512&rows=512
```

- Bounds are WGS84 longitude/latitude in `west,south,east,north` order with `west < east` and `south < north`. Longitude must be within `[-180,180]`, latitude within `[-85,85]`; antimeridian-crossing requests are not supported.
- Each of `rows` and `columns` is in `1..512`. Subdivide larger analysis regions into adjacent windows with consistently aligned cell centers.
- Each output cell center samples the **containing native Copernicus GeoTIFF pixel** (nearest source pixel). This is not bilinear interpolation or the normalized 65 × 65 heightfield.
- Successful HTTP responses contain **little-endian IEEE-754 float32** samples, row-major from north to south, `rows × columns × 4` bytes, MIME `application/vnd.aowis.dem-f32le`.
- Missing native source pixels are quiet NaNs in otherwise successful grids; completely unavailable windows return `404`.
- Dataset datum is unchanged (EGM2008 for GLO-30); no vertical transformation is applied.

The in-process equivalent uses the same provider implementation:

```cpp
Aowis::Map::TerrainAnalysisGridRequest request;
request.dataset = QStringLiteral("copernicus-glo30");
request.bounds = {8.10, 50.10, 8.11, 50.11}; // west, south, east, north
request.columns = 512;
request.rows = 512;
const Aowis::Map::TerrainAnalysisGridResult grid = map_terrain.analysisGrid(request);
```

For an in-process AOWIS Standalone/Hydrology integration, share the existing `TerrainData` instance rather than creating a second Map service or Copernicus download store. The Map subsystem owns provider data and retrieval; Hydrology owns its analysis jobs and results.

## Binary terrain format

The renderer-oriented `.aowterrain` v1 format is documented separately in the [tile format specification](terrain-tile-format.md). It encodes a 65 × 65 heightfield with quantized uint16 samples, a no-data sentinel, projection and vertical-datum metadata, dataset/XYZ identity and payload CRC. A normalizer must record the **actual** source datum; unknown must remain unknown rather than inventing a conversion. Formats and datasets have independent version/identity semantics.
