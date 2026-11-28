# Permanent global ETOPO data

The standard MAP distribution includes NOAA ETOPO 2022 v1, 60 arc-second
ice-surface elevations (21600 × 10800 samples), plus the existing visual
Web Mercator overview for z0–7. Both are permanent application data, not cache
entries. The original retains its full resolution, bathymetry and negative
land elevations. The visual overview clamps negative elevations to sea level,
including negative land elevations. Its records do not claim native Copernicus
accuracy. This stage does not change engineering elevation or analysis queries.

Source: <https://www.ncei.noaa.gov/products/etopo-global-relief-model>.
CC0-1.0; see [data attribution](etopo-copyright). The exact source URL, local
SHA-256 digests, datum, license and derivation are recorded in the data manifest.
The digests verify local reuse, not an independently published NOAA checksum.

## Prepare once and reuse

`compile_linux.sh`, `compile_deb.sh` and `compile_deb_docker.sh` automatically
invoke `tools/prepare_global_terrain_data.py`. It uses a persistent directory:

```
${XDG_DATA_HOME:-$HOME/.local/share}/aowis-server-map/datasets/etopo-2022-v1-60s/
```

Override with `AOWIS_MAP_DATA_BUILD_CACHE` (outside the source/build trees).
The original is downloaded from NOAA only if no local source exists. A file
already downloaded into the MAP checkout under its official filename is
imported automatically, including during Docker builds. For an original stored
elsewhere, explicitly import it once:

```bash
python3 tools/prepare_global_terrain_data.py --offline --source /path/to/ETOPO_2022_v1_60s_N90W180_surface.tif
```

On Arch, use an isolated Python environment (outside the checkout):

```bash
sudo pacman -S --needed python python-pip curl
python3 -m venv "$HOME/.local/share/aowis-server-map/terrain-python"
source "$HOME/.local/share/aowis-server-map/terrain-python/bin/activate"
python -m pip install numpy rasterio pyproj
```

Keep the environment active when running native build scripts. Debian's native
dependency installer and Docker toolchain include the Python dependencies.
Before source acquisition or regeneration, the preparation script checks its
Python dependencies. A completed `.tif.part` from an earlier failed run is
validated block by block and promoted without any network request, including
with `--offline`. An incomplete file is retained for resuming; offline mode
never invokes curl. Building the
first overview requires working disk space and RAM for raster reprojection;
subsequent builds verify and reuse the existing artifacts without reprojection.
The preparation tool is serialized by a file lock. A missing/corrupt derived pack
is rebuilt locally; an original changed since its manifest was written is
reported as an error rather than replaced. `--offline` prevents acquisition of
a missing original. The source and pack are not included in source ZIPs or Git.

Development builds link the prepared files into `build-linux/terrain-data/`.
`./compile_linux.sh --no-global-data --no-run` is a code-only build; the normal
unit-test build never prepares or downloads data. CMake itself remains free of
network or dataset dependencies.

## Debian installation

```bash
./compile_deb_docker.sh
```

Two packages are produced in `dist/`:

- `aowis-server-map_<code-version>_amd64.deb` (architecture varies).
- `aowis-server-map-data_2022.1+overview1_all.deb` (architecture independent).

On a new server, transfer both artifacts and install both in one `apt install`
transaction. For example (replace the code package version if different):

```bash
sudo apt install ./aowis-server-map-data_2022.1+overview1_all.deb ./aowis-server-map_0.1.0+deb1_amd64.deb
```

The server package requires the data package. The separately versioned data
package remains installed on routine server updates; distribute just the new
server package while the required data version is already installed. Do not
redownload ETOPO on each server. An explicit future data format/source revision
must change the data package version and dependency together. The package owns
`/usr/share/aowis-server-map/terrain/`; cache eviction and terrain cache deletion
do not operate on that directory. No network requests are made by installation
scripts or by the runtime to obtain ETOPO.

Runtime searches for the administrator's explicitly provisioned overview first,
then `<executable-dir>/terrain-data/`, then the installed share directory
(relative to the executable). It validates the pack header and each delivered
record. With a valid pack, `GET /terrain/v1/datasets` advertises `minimum_zoom: 0`,
`native_minimum_zoom: 8`, `maximum_zoom: 14`. A z0 tile can be served locally
without provider downloads. Native Copernicus and raster cache paths do not
change. No third-party cache invalidation is necessary.

For portable/standalone deployment, the data files can be installed beside the
executable under `terrain-data/`. The separate AOWIS-Standalone installer and
runtime container image build systems are not changed by this MAP patch.
