# AOWIS normalized terrain tile format

## Purpose

AOWIS terrain providers normalize their source data into one provider-neutral binary heightfield format before the data is used by either:

- point-elevation sampling for entity inspectors;
- terrain geometry in the native RHI 3D map;
- offline terrain packages.

Provider-native formats such as HGT, GeoTIFF/COG or a future local DEM importer must not leak into these consumers.

Format version 1 uses the file extension `.aowterrain` and MIME type `application/vnd.aowis.terrain`.

## Addressing and projection

Each terrain tile is addressed as standard Web-Mercator XYZ:

```text
z/x/y
```

The projection is Web Mercator / EPSG:3857 with the usual slippy-map orientation. Consequently v1 covers the standard Web-Mercator latitude range of approximately `-85.05112878° ... +85.05112878°`; a future format/projection identifier can extend terrain storage beyond that range without changing v1 semantics.


- X increases west-to-east;
- Y increases north-to-south;
- zoom `z` contains `2^z` tiles on each axis;
- valid X and Y are in `[0, 2^z - 1]`;
- format v1 accepts zoom levels 0 through 30.

The normalized cache path is:

```text
normalized/v1/<dataset>/<z>/<x>/<y>.aowterrain
```

Dataset identifiers are stable lowercase ASCII identifiers containing only letters, digits, `-`, `_` and `.`, beginning with a letter or digit. Examples are `copernicus-glo30` and `nasadem`.

## Heightfield geometry

Every v1 tile contains exactly a **65 × 65** regular elevation grid:

```text
65 samples × 65 samples
= 64 × 64 terrain cells
= 4225 elevation samples
```

Samples are row-major:

1. north-to-south rows;
2. west-to-east samples inside each row.

Both tile boundaries are included. For grid column `c` and row `r`, where each is in `[0, 64]`, the sample is located at the fractional XYZ coordinate:

```text
x = tile_x + c / 64
y = tile_y + r / 64
```

converted through inverse Web Mercator at zoom `z`.

Consequently adjacent normalized tiles share their boundary coordinates exactly. Normalizers must sample a common boundary coordinate consistently so adjacent tiles receive matching edge elevations. The later renderer may still use skirts or another seam strategy while different LODs meet.

The 65×65 choice is intentional: the renderer can reuse one static 64×64-cell index buffer for every terrain tile, while only the height samples change.

## Elevation encoding

The in-memory `TerrainTile` uses `float` metres and represents no-data as NaN.

The binary payload uses little-endian unsigned 16-bit values:

- `0 ... 65534`: valid quantized elevation;
- `65535`: no-data.

Each tile stores:

```text
minimum_elevation_m
elevation_scale_m
```

A valid code is decoded as:

```text
elevation_m = minimum_elevation_m + code * elevation_scale_m
```

For a non-flat tile the encoder chooses:

```text
elevation_scale_m =
    (maximum_elevation_m - minimum_elevation_m) / 65534
```

so the full valid integer range is used. For a flat tile the scale is zero and every valid sample uses code zero.

This quantization is deliberately much more precise than global 30 m DEM source accuracy. A tile with 1000 m of vertical range has a quantization step of about 1.5 cm.

A normalized tile containing no finite elevation samples is invalid and must not be stored as a successful terrain tile.

## Binary layout

All multibyte integer and IEEE-754 float fields are **little-endian**.

The fixed header is 64 bytes. It is followed immediately by the UTF-8 dataset identifier and then the elevation payload.

| Offset | Size | Type | Field | v1 value / meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | bytes | magic | `AOWTRN\0\0` |
| 8 | 2 | uint16 | format version | `1` |
| 10 | 2 | uint16 | header bytes | `64 + dataset_id_bytes` |
| 12 | 4 | uint32 | flags | `0` |
| 16 | 1 | uint8 | projection | `1` = Web-Mercator XYZ |
| 17 | 1 | uint8 | encoding | `1` = uint16 linear |
| 18 | 1 | uint8 | vertical datum | see below |
| 19 | 1 | uint8 | reserved | `0` |
| 20 | 2 | uint16 | grid width | `65` |
| 22 | 2 | uint16 | grid height | `65` |
| 24 | 1 | uint8 | zoom | `0 ... 30` |
| 25 | 3 | bytes | reserved | zero |
| 28 | 4 | uint32 | tile X | XYZ X |
| 32 | 4 | uint32 | tile Y | XYZ Y |
| 36 | 4 | float32 | minimum elevation | metres |
| 40 | 4 | float32 | elevation scale | metres per code |
| 44 | 4 | float32 | nominal resolution | source resolution in metres, `0` if genuinely unknown |
| 48 | 2 | uint16 | dataset ID bytes | UTF-8 byte count |
| 50 | 2 | uint16 | reserved | `0` |
| 52 | 4 | uint32 | sample count | `4225` |
| 56 | 4 | uint32 | payload bytes | `8450` |
| 60 | 4 | uint32 | payload CRC32 | standard IEEE CRC32 of payload only |
| 64 | variable | UTF-8 | dataset ID | canonical dataset identifier |
| header bytes | 8450 | uint16[] | height payload | 4225 row-major codes |

The v1 decoder rejects unsupported flags, projection/encoding identifiers, dimensions, malformed addresses, invalid metadata, bad CRCs, unexpected trailing bytes and unknown vertical-datum numeric codes.

## Vertical datum codes

Binary vertical-datum codes map to the stable elevation contract:

| Code | Identifier |
| ---: | --- |
| 0 | `unknown` |
| 1 | `wgs84-ellipsoid` |
| 2 | `egm96` |
| 3 | `egm2008` |
| 4 | `local` |

Reference semantics are explicit:

- `wgs84-ellipsoid` is ellipsoidal height carried by WGS 84 3D (`EPSG:4979`);
- `egm96` is gravity-related/orthometric height (`EPSG:5773`);
- `egm2008` is gravity-related/orthometric height (`EPSG:3855`);
- `local` means only that the source uses a local/project vertical reference. It does not establish compatibility with another `local` dataset.

A normalizer must record the actual datum of its source. It must not infer or invent a datum merely from a provider name. Unknown remains a legitimate explicit value when the source datum cannot be established. Invalid enum/numeric values are rejected rather than silently encoded as `unknown`.

## Integrity and versioning

The payload carries a standard IEEE CRC32. This is intended to detect truncated or corrupted cached/offline tile data cheaply before decoding or GPU upload.

The v1 file itself is intentionally not internally compressed. HTTP transport may use content encoding and offline packages may compress their members. Keeping the canonical cache tile uncompressed makes decoding, random access and future direct GPU-oriented staging simple. A typical v1 payload is only 8450 bytes before transport/package compression.

The format version is independent of provider/dataset versions. A new source revision of `copernicus-glo30`, for example, does not require an AOWIS format-version change if it still normalizes into the same v1 structure.

An incompatible binary/layout change requires a new AOWIS terrain format version and a separate normalized cache namespace such as `normalized/v2/...`. Existing v1 meanings must not be changed in place.

## What is intentionally not in v1

The normalized tile does not contain:

- imagery or landclass textures;
- triangle indices;
- normals;
- provider-native metadata blobs;
- download/cache origin;
- HTTP metadata;
- hydraulic/project elevations.

The renderer can derive vertices/normals and reuse static topology. Imagery remains the separate raster-map subsystem. Cache origin describes how a tile was obtained and is runtime metadata, not an intrinsic property of the normalized terrain dataset.

## Renderer delivery endpoint

`AOWIS-SERVER-MAP` exposes cached normalized tiles directly to renderers:

```text
GET /terrain/v1/<dataset>/<z>/<x>/<y>.aowterrain
```

A successful response has MIME type:

```text
application/vnd.aowis.terrain
```

The response body is the canonical `.aowterrain` file bytes stored in the normalized cache. The server decodes and validates the file before delivery, including payload CRC and the embedded dataset/XYZ address. A tile whose embedded metadata does not match the requested cache path is treated as corrupt and is not delivered.

Current response semantics are:

- `200`: valid cached normalized tile;
- `400`: invalid dataset identifier or XYZ address;
- `401`: missing/invalid server read API key when authentication is enabled;
- `404`: no cached tile exists and no enabled provider can supply the requested dataset/address;
- `500`: cached tile is unreadable/corrupt or provider normalization failed locally;
- `502`: an enabled remote provider failed to fetch its upstream source;
- `503`: terrain subsystem is disabled or not initialized.

`OPTIONS` is also accepted for the same route. Provider fetch-on-miss is implemented above this format layer: eligible misses may be generated and atomically persisted without changing either the renderer URL or binary format.

