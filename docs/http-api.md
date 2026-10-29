# HTTP API reference

All routes use the configured server host and port (default `http://127.0.0.1:8122`). The server speaks HTTP through Qt 6 QHttpServer. For external access place it behind a trusted TLS reverse proxy rather than exposing the backend listener directly.

- **Read authentication:** GET endpoints require `authentication/api_key` when that key is configured; otherwise read requests are open in user mode.
- **Administrative authentication:** cache DELETE and upstream cancellation endpoints require `authentication/delete_api_key`. If that key is empty, those operations are **disabled**, not anonymous.
- **Provider editing exception:** `PUT /map/v1/providers` has **no API-key check**. Restrict it to trusted clients at the network/proxy boundary. CORS alone does not restrict access.
- For authenticated requests, send `X-API-Key: <appropriate-key>` or `Authorization: Bearer <appropriate-key>`.

Except where noted, successful GETs use HTTP `200`. `OPTIONS` is supported for the registered routes. A full request/admission queue returns HTTP `503`; terrain admission paths may include `Retry-After: 1`.

## Endpoints

| Method | Route | Result |
| --- | --- | --- |
| GET | `/status` | Plain-text Qt/server and terrain status |
| GET | `/map/v1/providers` | JSON raster-provider catalog |
| PUT | `/map/v1/providers` | Replace and persist provider catalog (**unauthenticated**) |
| GET | `/<provider-uuid>/<z>/<x>/<y>.png` | `image/png` raster tile |
| DELETE | `/cache/<provider-uuid>/<z>/<xmin>/<xmax>/<ymin>/<ymax>` | Number of deleted raster cache entries |
| GET | `/upstream/v1/activity` | Raster and terrain `active`/`queued` counts |
| DELETE | `/upstream/v1/map-tiles` | Cancel outstanding raster downloads |
| DELETE | `/upstream/v1/terrain` | Cancel outstanding terrain downloads |
| GET | `/terrain/v1/datasets` | JSON terrain dataset catalog |
| GET | `/terrain/v1/<dataset>/<z>/<x>/<y>.aowterrain` | Canonical normalized heightfield bytes |
| GET | `/terrain/v1/elevation?latitude=…&longitude=…` | JSON point elevation |
| GET | `/terrain/v1/analysis/grid?dataset=…&west=…&south=…&east=…&north=…&columns=…&rows=…` | `application/vnd.aowis.dem-f32le` |
| DELETE | `/terrain/v1/cache/<dataset>/<z>/<xmin>/<xmax>/<ymin>/<ymax>` | Delete normalized tiles in range |
| DELETE | `/terrain/v1/source/<dataset>/<z>/<xmin>/<xmax>/<ymin>/<ymax>` | Delete native COG source cells intersecting range |

In routes, `<provider-uuid>` is an imagery provider UUID (for example, OpenStreetMap's `0381bc44-67e9-5af9-aeb0-023ccba36658`); `<dataset>` is a terrain identifier such as `copernicus-glo30`. The two identifier namespaces are different.

## Service status and activity

```bash
curl -fsS http://127.0.0.1:8122/status
curl -fsS http://127.0.0.1:8122/upstream/v1/activity
```

Activity is JSON with `map_tiles` and `terrain` objects, each containing `active` and `queued` integer counts. These are upstream activity counters, not a complete dump of every admitted terrain job or a memory usage metric.

## Raster map tiles

```bash
curl -f -o tile.png \
  'http://127.0.0.1:8122/0381bc44-67e9-5af9-aeb0-023ccba36658/8/133/86.png'
```

The provider must be configured, enabled and support the requested XYZ zoom. Cached hits are served without upstream fetching; absent tiles may be downloaded if the global/provider/coverage policies permit it. Missing or disallowed raster requests currently map to `400` through the invalid-request path; the raster download queue can return `503`.

By default, eligible cache misses can wait for the asynchronous fetch. To ask for immediate cache-first delivery:

```bash
curl -i 'http://127.0.0.1:8122/0381bc44-67e9-5af9-aeb0-023ccba36658/8/133/86.png?delivery=cache-first'
```

If the tile is already available, the result is `200`. If fetching is in progress, the result may be **`202 Accepted`** with `Map tile is being fetched` instead of holding a waiter. The equivalent request header is `X-AOWIS-Tile-Delivery: cache-first`. This is a *response-delivery preference*, not a prefetch API.

### Raster provider management

```bash
curl -fsS http://127.0.0.1:8122/map/v1/providers
```

The response is a JSON object with a `providers` array; each entry contains `id`, `name`, `url`, `minimum_zoom`, `maximum_zoom`, `enabled`, `remote_fetch_enabled` and `coverage`. Disabled providers are included and configured URLs are visible.

A PUT replaces **the entire catalog**, not just selected entries:

```bash
curl -i -X PUT 'http://127.0.0.1:8122/map/v1/providers' \
  -H 'Content-Type: application/json' \
  --data-binary '{
    "providers": [{
      "id": "0381bc44-67e9-5af9-aeb0-023ccba36658",
      "name": "OpenStreetMap",
      "url": "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
      "minimum_zoom": 0,
      "maximum_zoom": 19,
      "enabled": true,
      "remote_fetch_enabled": true,
      "coverage": ""
    }]
  }'
```

Up to 100 providers are accepted. The server validates the UUIDs, URL templates and zoom limits before updating the in-memory catalog and atomically saving `providers.ini`. It **never rewrites the administrator's `map-server.ini`**. The `providers` array can be intentionally empty. A URL change separates the disk-cache variant so imagery from the previous upstream is not silently reused.

> The PUT endpoint currently has **no read-key or delete-key authentication**. Apply trusted-client restrictions before deploying this feature on any shared network.

## Terrain datasets and normalized tiles

```bash
curl -fsS http://127.0.0.1:8122/terrain/v1/datasets
curl -f -o tile.aowterrain \
  http://127.0.0.1:8122/terrain/v1/copernicus-glo30/8/133/86.aowterrain
```

The dataset catalog describes supported terrain sources, zoom ranges, nominal resolution, datum and default dataset. Copernicus GLO-30 is a 30 m nominal DSM in EGM2008 and supports **on-demand normalized tile generation at zooms 8–14**. Normalized `65 × 65` terrain tiles have content type `application/vnd.aowis.terrain`; they carry their own dataset/XYZ identity and payload CRC. The binary structure is defined in the [tile format specification](terrain-tile-format.md).

A tile missing from the normalized cache can be generated from cached native COGs or newly downloaded source data when permitted. An unavailable tile returns `404`, an upstream fetch failure `502`, and a malformed dataset/address `400`. Corrupt tiles or local provider errors use `500`; disabled/uninitialized terrain uses `503`. **Don't mistake a valid route returning 404 for a successful cache benchmark.**

## Point elevations

```bash
curl -fsS \
  'http://127.0.0.1:8122/terrain/v1/elevation?latitude=50.1&longitude=8.1'
```

`latitude` and `longitude` are WGS84 decimal degrees. An optional `vertical_datum` may be `native`, `wgs84-ellipsoid`, `egm96` or `egm2008`. The dataset is taken from the configured `terrain/default_dataset` (not from the URL). Response fields include `status`, `elevation_m`, `dataset`, `nominal_resolution_m`, `vertical_datum`, `source_vertical_datum`, `origin`, and the sampled `tile` address. See the [elevation contract](terrain-elevation-contract.md) for stable identifiers, full JSON shape and status mapping.

Only **identity** vertical-datum requests are supported. A requested datum requiring a geoid conversion is rejected (`400`) rather than returning a falsely relabeled height. GLO-30 is a surface model, not surveyed hydraulic elevation.

## Float32 analysis grids

The grid route returns the **native GeoTIFF source pixel containing each output cell center**. It does not resample the quantized 65 × 65 normalized renderer tiles.

```bash
curl -f -o dem.f32le \
  'http://127.0.0.1:8122/terrain/v1/analysis/grid?dataset=copernicus-glo30&west=8.10&south=50.10&east=8.11&north=50.11&columns=512&rows=512'
```

Request bounds are WGS84 degrees in `west,south,east,north` order: `-180 ≤ west < east ≤ 180` and `-85 ≤ south < north ≤ 85`. The request must not cross the antimeridian. `columns` and `rows` must each be in `1..512`. The `dataset` argument is optional and defaults to `terrain/default_dataset`.

Successful response: **`application/vnd.aowis.dem-f32le`**, unframed little-endian IEEE-754 float32 samples in row-major **north-to-south** order, exactly `columns × rows × 4` bytes (512 × 512 = 1,048,576 bytes). Missing/NoData source cells become quiet NaNs within an otherwise valid window. A wholly unavailable window returns HTTP `404`.

For larger windows, request adjacent subdivisions with cell centers aligned to the same full-grid geometry. The endpoint does not transform EGM2008 elevations into WGS84 ellipsoid heights. The same operation is available in-process through `Aowis::Map::TerrainData::analysisGrid()`.

## Cache deletion and upstream cancellation

Delete requests require the **delete API key**:

```bash
curl -i -X DELETE -H 'X-API-Key: YOUR_DELETE_KEY' \
  'http://127.0.0.1:8122/terrain/v1/cache/copernicus-glo30/8/133/133/86/86'
```

- Raster cache deletion removes cached raster tiles from an XYZ range; administrator-provisioned **permanent tiles are not deleted**.
- Terrain cache deletion removes normalized `.aowterrain` tiles, which can subsequently be regenerated from still-cached native COGs.
- Terrain source deletion removes the native source cells needed by the specified XYZ range. A 1° source COG can be shared with other terrain tiles, so this may affect future normalization beyond that range.
- `DELETE /upstream/v1/map-tiles` and `DELETE /upstream/v1/terrain` request cancellation of the corresponding upstream operations, not automatic cache deletion.

Terrain deletion runs outside the HTTP event loop. Active source file locks and overlapping cache writes can delay deletion. Invalid deletion requests return `400`; errors may return `500`; admission pressure may produce `503`.

## Response and safety notes

| Status | Typical meaning |
| --- | --- |
| `200` | Requested response completed |
| `202` | Cache-first raster tile still being fetched |
| `400` | Bad parameters, invalid raster request or unsupported datum conversion |
| `401` | Invalid/missing required API key |
| `404` | Requested terrain/source data unavailable |
| `500` | Local provider, cache or internal failure |
| `502` | Remote terrain source acquisition failure |
| `503` | Disabled terrain, uninitialized subsystem or admission overload |

Cache hits remain usable in offline mode. Separate `downloads/remote_fetch_enabled` and `terrain/remote_fetch_enabled` switches control **new** upstream acquisition. An explicit geographical coverage policy restricts downloads, not what cached data may be served.
