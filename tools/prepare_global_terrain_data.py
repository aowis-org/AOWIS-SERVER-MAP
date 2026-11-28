#!/usr/bin/env python3
"""Prepare the permanent, versioned ETOPO distribution data (Linux build host).
No downloads at runtime. Keep the original GeoTIFF across all rebuilds.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

SOURCE = 'ETOPO_2022_v1_60s_N90W180_surface.tif'
URL = 'https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/60s_surface_elev_gtif/' + SOURCE
PACK = 'global-overview-v1.aowpack'
VERSION = '2022.1+overview1'
MANIFEST = 'etopo-2022-provenance.json'
DEB = f'aowis-server-map-data_{VERSION}_all.deb'
RECORD_BYTES = 8614
TILE_COUNT = 21845


def cache_directory():
    base = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share')
    return Path(os.environ.get('AOWIS_MAP_DATA_BUILD_CACHE') or
                base / 'aowis-server-map/datasets/etopo-2022-v1-60s').expanduser().resolve()


def sha256(path):
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
        return digest.hexdigest()


def validate_source(path):
    # Check the full raster, not just its filename or TIFF magic bytes.
    import rasterio
    from pyproj import CRS
    with rasterio.open(path) as src:
        if (src.width, src.height, src.count) != (21600, 10800, 1):
            raise ValueError('expected full global 60 arc-second ETOPO raster')
        if src.crs is None:
            raise ValueError('ETOPO source has no CRS')
        crs = CRS.from_wkt(src.crs.to_wkt())
        horizontal = crs.sub_crs_list[0] if crs.is_compound else crs
        if (not horizontal.equals(CRS.from_epsg(4326), ignore_axis_order=True)
                or any(abs(a-b) > 1e-5 for a,b in zip(src.bounds, (-180,-90,180,90)))):
            raise ValueError('unexpected ETOPO geographic reference or extent')
        if crs.is_compound and not crs.sub_crs_list[1].equals(CRS.from_epsg(3855)):
            raise ValueError('expected EGM2008 vertical datum')
        # The overview builder also reads every source pixel and rejects gaps.


def validate_pack(path):
    if path.stat().st_size != 16 + TILE_COUNT * RECORD_BYTES:
        raise ValueError('invalid overview package length')
    with path.open('rb') as stream:
        if stream.read(16) != struct.pack('<8sII', b'AOWCOAR1', 7, RECORD_BYTES):
            raise ValueError('invalid overview package header')
        for z in range(8):
            for y in range(1 << z):
                for x in range(1 << z):
                    record = stream.read(RECORD_BYTES)
                    if (record[:8] != b'AOWTRN\0\0' or record[24] != z
                            or struct.unpack_from('<II', record, 28) != (x,y)
                            or record[64:80] != b'copernicus-glo30'
                            or struct.unpack_from('<H', record, 8)[0] != 2
                            or zlib.crc32(record[80:]) != struct.unpack_from('<I', record, 60)[0]):
                        raise ValueError(f'invalid overview record {z}/{x}/{y}')


def atomic_json(path, data):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + '\n')
    os.replace(temporary, path)


def prepare(directory, local_source=None, offline=False):
    source, pack, manifest = (directory / name for name in (SOURCE, PACK, MANIFEST))
    old = json.loads(manifest.read_text()) if manifest.exists() else None
    if old is not None and old.get('version') != VERSION:
        raise ValueError('unexpected data manifest version; use a separate dataset directory')
    if source.exists():
        print(f'Reusing original ETOPO: {source}', flush=True)
    elif local_source is not None:
        validate_source(local_source)
        temporary = directory / (SOURCE + '.import')
        shutil.copyfile(local_source, temporary)
        os.replace(temporary, source)
    else:
        if offline:
            raise ValueError('ETOPO source missing and --offline was specified')
        temporary = directory / (SOURCE + '.part')
        subprocess.run(['curl', '--fail', '--location', '--retry', '3',
                        '--connect-timeout', '30', '--continue-at', '-',
                        '--output', str(temporary), URL], check=True)
        validate_source(temporary)
        # Only publish a source that can be opened; never overwrite an existing one.
        os.replace(temporary, source)
    source_hash = sha256(source)
    if old is not None and old['source_sha256'] != source_hash:
        raise ValueError('stored ETOPO source changed; refusing automatic replacement')
    if old is not None and pack.exists() and old['overview_sha256'] == sha256(pack):
        print(f'Reusing global overview: {pack}', flush=True)
        return old
    validate_source(source)
    # Build again only if the derived data is missing, invalid or not tied to this source.
    subprocess.run([sys.executable, str(Path(__file__).with_name('build_global_terrain_overview.py')),
                    str(source), str(pack), '--source-resolution-m', '1855', '--clamp-negative'], check=True)
    validate_pack(pack)
    pack.chmod(0o644)
    result = dict(version=VERSION, source=SOURCE, source_url=URL, source_sha256=source_hash,
                  overview=PACK, overview_sha256=sha256(pack), license='CC0-1.0',
                  attribution='NOAA National Centers for Environmental Information, ETOPO 2022',
                  datum='EGM2008', resolution_arc_seconds=60,
                  overview_processing='Web Mercator z0-7, area averaging, negative elevations clamped to zero for visualization')
    atomic_json(manifest, result)
    return result


def build_deb(directory, provenance):
    output = directory / DEB
    stamp = directory / (DEB + '.json')
    if output.exists() and stamp.exists():
        cached = json.loads(stamp.read_text())
        if cached.get('provenance') == provenance and cached.get('sha256') == sha256(output):
            print(f'Reusing data package: {output}', flush=True)
            return output
    with tempfile.TemporaryDirectory(prefix='data-deb-', dir=directory) as work:
        root = Path(work) / 'root'
        control = root / 'DEBIAN'
        control.mkdir(parents=True)
        (control / 'control').write_text(
            f'Package: aowis-server-map-data\nVersion: {VERSION}\nArchitecture: all\n'
            'Section: misc\nPriority: optional\nMaintainer: AOWIS Project <aowis@aowis.org>\n'
            'Description: Permanent global ETOPO terrain data for AOWIS MAP\n'
            ' Contains NOAA ETOPO 2022 60 arc-second source and visual terrain overview.\n')
        data = root / 'usr/share/aowis-server-map/terrain'
        data.mkdir(parents=True)
        for name in (SOURCE, PACK, MANIFEST):
            shutil.copyfile(directory / name, data / name)
            (data / name).chmod(0o644)
        doc = root / 'usr/share/doc/aowis-server-map-data'
        doc.mkdir(parents=True)
        shutil.copyfile(Path(__file__).resolve().parents[1] / 'docs/etopo-copyright', doc / 'copyright')
        temporary = Path(work) / DEB
        subprocess.run(['dpkg-deb', '--root-owner-group', '-Zzstd', '--build', str(root), str(temporary)], check=True)
        os.replace(temporary, output)
    atomic_json(stamp, dict(provenance=provenance, sha256=sha256(output)))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=cache_directory())
    parser.add_argument('--source', type=Path, help='import an already downloaded original; never replace a cached source')
    parser.add_argument('--offline', action='store_true', help='fail instead of acquiring a missing original')
    parser.add_argument('--deb-output', type=Path, help='export the independently versioned Debian data package')
    parser.add_argument('--link-directory', type=Path, help='expose permanent data beside a development executable')
    args = parser.parse_args()
    local_source = args.source
    if local_source is None:
        # Reuse an original downloaded with the earlier manual provisioning
        # instructions. The Docker wrapper points to the read-only checkout.
        candidate = Path(os.environ.get('AOWIS_MAP_ETOPO_SOURCE') or
                         Path(__file__).resolve().parents[1] / SOURCE)
        if candidate.is_file():
            local_source = candidate
    directory = args.cache.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / '.prepare.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        provenance = prepare(directory, local_source, args.offline)
        if args.deb_output is not None:
            package = build_deb(directory, provenance)
            args.deb_output.mkdir(parents=True, exist_ok=True)
            dest = args.deb_output / package.name
            if dest.resolve() != package.resolve() and (not dest.exists() or sha256(dest) != sha256(package)):
                temporary = dest.with_suffix('.tmp')
                shutil.copyfile(package, temporary)
                os.replace(temporary, dest)
        if args.link_directory is not None:
            args.link_directory.mkdir(parents=True, exist_ok=True)
            for name in (SOURCE, PACK, MANIFEST):
                dest = args.link_directory / name
                if dest.is_symlink() and dest.resolve() == directory / name:
                    continue
                if dest.exists() or dest.is_symlink():
                    raise ValueError(f'refusing to replace existing data: {dest}')
                dest.symlink_to(directory / name)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, ImportError) as error:
        sys.exit(f'ETOPO provisioning failed: {error}')
