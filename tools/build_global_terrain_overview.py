#!/usr/bin/env python3
"""Build a local, visual-only z0..7 terrain overview from a global EGM2008 raster.

No network access. Requires numpy and rasterio (GDAL). The source must be a
north-up, global EPSG:4326 raster, with elevations in metres above EGM2008.
"""
import argparse
import os
from pathlib import Path
import struct
import tempfile
import zlib

import numpy as np
import rasterio
from rasterio.transform import from_bounds
from rasterio.warp import reproject, Resampling

DATASET = b"copernicus-glo30"
MAX_ZOOM = 7
GRID_CELLS = 64
METADATA_BYTES = 84
RECORD_BYTES = 64 + len(DATASET) + METADATA_BYTES + 65 * 65 * 2
HALF_WORLD_M = 20037508.342789244
# Fixed, exactly representable quantization ensures adjacent tiles decode their
# shared samples identically (including the antimeridian). No clipping.
MIN_M = -12000.0
SCALE_M = 0.5


def tile_offset(z, x, y):
    side = 1 << z
    return 16 + ((side * side - 1) // 3 + y * side + x) * RECORD_BYTES


def encode_tile(z, x, y, heights, resolution_m):
    if heights.shape != (65, 65) or not np.isfinite(heights).all():
        raise ValueError("overview tile must have 65x65 finite elevations")
    codes = np.floor((heights.astype(np.float64) - MIN_M) / SCALE_M + 0.5)
    if codes.min() < 0 or codes.max() > 65534:
        raise ValueError("overview elevations outside fixed encoding range")
    # We do not pretend the overview is native Copernicus GLO-30 or claim
    # that residuals against a coarse source bound the native terrain error.
    payload = bytes(METADATA_BYTES) + codes.astype('<u2').tobytes()
    header = struct.pack('<8sHHI4BHHB3xIIfffHHIII', b'AOWTRN\0\0', 2,
                         64 + len(DATASET), 0, 1, 1, 3, 0, 65, 65,
                         z, x, y, MIN_M, SCALE_M, resolution_m,
                         len(DATASET), 0, 65 * 65, len(payload),
                         zlib.crc32(payload))
    assert len(header) == 64
    return header + DATASET + payload


def vertices(cells, x, y):
    """Average the four neighboring cell means. Wrap longitude, clamp latitude."""
    n = cells.shape[0]
    rows = np.arange(y * 64, y * 64 + 65)
    cols = np.arange(x * 64, x * 64 + 65)
    top, bottom = np.maximum(rows - 1, 0), np.minimum(rows, n - 1)
    left, right = (cols - 1) % n, cols % n
    return (cells[np.ix_(top, left)] + cells[np.ix_(top, right)]
            + cells[np.ix_(bottom, left)] + cells[np.ix_(bottom, right)]) * 0.25


def write_pack(cells, output, resolution_m, max_zoom=MAX_ZOOM):
    if cells.shape != (64 << max_zoom, 64 << max_zoom):
        raise ValueError("unexpected projected grid size")
    if not np.isfinite(cells).all():
        raise ValueError("overview must cover all Web Mercator cells; input has gaps")
    tile_count = ((1 << (2 * (max_zoom + 1))) - 1) // 3
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + '.',
                                         suffix='.tmp', delete=False) as pack:
            temporary = Path(pack.name)
            pack.write(struct.pack('<8sII', b'AOWCOAR1', max_zoom, RECORD_BYTES))
            pack.truncate(16 + tile_count * RECORD_BYTES)
            for z in range(max_zoom, -1, -1):
                side = 1 << z
                pack.seek(tile_offset(z, 0, 0))
                for y in range(side):
                    for x in range(side):
                        pack.write(encode_tile(z, x, y, vertices(cells, x, y), resolution_m))
                print(f'Zoom {z}: {side * side} tiles', flush=True)
                if z:
                    # Area means in the projected grid; not a point-sampled pyramid.
                    cells = (cells[0::2, 0::2] + cells[0::2, 1::2]
                             + cells[1::2, 0::2] + cells[1::2, 1::2]) * 0.25
            pack.flush()
            os.fsync(pack.fileno())
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def project(source, clamp_negative, tempdir):
    with rasterio.open(source) as src:
        if (src.crs != rasterio.crs.CRS.from_epsg(4326)
                or src.transform.b != 0 or src.transform.d != 0
                or src.transform.a <= 0 or src.transform.e >= 0
                or any(abs(a - b) > 1e-5 for a, b in zip(src.bounds, (-180, -90, 180, 90)))):
            raise ValueError('source must be a north-up global EPSG:4326 raster')
        # ETOPO includes ocean bathymetry. Explicit opt-in to clamp negative
        # elevations BEFORE averaging. This also flattens sub-sea-level land.
        # A prepared land-only, sea-level-filled source is preferable.
        clean = Path(tempdir) / 'land.tif'
        profile = dict(driver='GTiff', width=src.width, height=src.height, count=1,
                       dtype='float32', crs=src.crs, transform=src.transform,
                       tiled=True, blockxsize=256, blockysize=256, compress='deflate',
                       BIGTIFF='IF_SAFER')
        with rasterio.open(clean, 'w', **profile) as dst:
            for _, window in dst.block_windows(1):
                values = src.read(1, window=window, masked=True).astype(np.float32)
                if np.ma.getmaskarray(values).any() or not np.isfinite(values.data).all():
                    raise ValueError('input contains missing elevations; fill oceans/gaps first')
                data = values.data
                if clamp_negative:
                    data = np.maximum(data, 0)
                dst.write(data, 1, window=window)
    n = GRID_CELLS << MAX_ZOOM
    cells = np.full((n, n), np.nan, dtype=np.float32)
    with rasterio.open(clean) as src:
        reproject(source=rasterio.band(src, 1), destination=cells,
                  dst_transform=from_bounds(-HALF_WORLD_M, -HALF_WORLD_M,
                                            HALF_WORLD_M, HALF_WORLD_M, n, n),
                  dst_crs='EPSG:3857', dst_nodata=np.nan,
                  resampling=Resampling.average, warp_mem_limit=128)
    return cells


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='local global EGM2008 GeoTIFF')
    parser.add_argument('output', type=Path, help='global-overview-v1.aowpack')
    parser.add_argument('--source-resolution-m', type=float, required=True)
    parser.add_argument('--clamp-negative', action='store_true',
                        help='visual-only: clamp ocean bathymetry AND negative land before averaging')
    args = parser.parse_args()
    if not np.isfinite(args.source_resolution_m) or args.source_resolution_m <= 0:
        parser.error('source resolution must be positive and finite')
    if args.source.resolve() == args.output.resolve():
        parser.error('source and output must be different files')
    with tempfile.TemporaryDirectory(prefix='aowis-overview-') as tempdir:
        cells = project(args.source, args.clamp_negative, tempdir)
        write_pack(cells, args.output, args.source_resolution_m)
    print(f'Wrote {args.output} ({args.output.stat().st_size:,} bytes)')


if __name__ == '__main__':
    main()
