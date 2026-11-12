# MAP-to-MAP cascading (protocol and test specification)

**Status:** MAP-to-MAP raster and normalized-terrain cascading, zstd terrain transfer and adaptive JPEG imagery transfer are implemented. This document records the wire protocol, policy and acceptance criteria; it is not a deployment walkthrough. See [Configuration](configuration.md) for settings, paths and precedence. Bitkine cached-payload benchmarks are available; real WAN performance remains unmeasured. Native Copernicus GeoTIFF replication and hydrology-specific data are out of scope.

## Configuration and acquisition order

Administrator INI schema:

```ini
[upstream]
servers=primary,backup
allow_direct_fetch=false

[upstream.primary]
url=https://maps-primary.example.org
api_key=

[upstream.backup]
url=https://maps-backup.example.org
api_key=

[bandwidth]
poor_internet_mode=true
jpeg_quality=75
adaptive_compression=true
```

- `servers` is an ordered list of named AOWIS MAP upstreams, with per-server URL and read API key. Empty list means no MAP upstreams. Validate names, URLs and duplicates at startup; never log credentials.
- `allow_direct_fetch` independently controls whether original raster providers or Copernicus may be contacted **after all MAP upstreams fail**. Default `true` retains present behavior when no upstream is configured. Setting it `false` also permits an intentionally cache-only server with no upstreams.
- `poor_internet_mode` defaults to `false`. It optimizes **MAP-to-MAP transfers only**, not GUI-facing tile responses. JPEG quality defaults to 75. Adaptive compression defaults to enabled when poor-internet mode is enabled.
- Existing `downloads/remote_fetch_enabled`, `terrain/remote_fetch_enabled`, provider-level switches and geographic coverage remain authoritative gates: disabling remote fetching also disables MAP upstream requests for the affected data. `allow_direct_fetch` cannot override these gates.
- The existing read and delete API-key roles remain separate. Upstream connections use the upstream's **read** key only. Prefer HTTPS across WAN; reject cross-origin redirects and do not silently route requests to public tile services.

On a cache miss, for **both** raster XYZ and `.aowterrain` tiles:

1. Check local permanent/raster/normalized caches first; serve hits even while offline.
2. If remote fetching is permitted for that tile, request the *same rendering asset* from the first configured MAP upstream, then successive upstreams on failure/miss, in listed order.
3. Only if every MAP upstream fails, and `allow_direct_fetch=true`, use the existing original-provider/Copernicus acquisition path.
4. Otherwise return the existing appropriate unavailable/error response; never try an unconfigured origin.

Apply bounded timeouts, retry limits, deduplicated concurrent requests and short per-upstream failure cooldowns. A failed upstream must not stall unrelated cached requests. Detect cascade loops (e.g. A → B → A) with a hop limit **and** propagated visited-server identity; never use direct fetch to escape a loop when disabled. A cascade may have any number of levels within that safety limit.

For Step 3 terrain requests, upstreams exchange `X-AOWIS-Cascade-Visited` and optional `X-API-Key`, and successful `.aowterrain` responses carry `X-AOWIS-Terrain-Revision: 2`; downstreams reject missing or incompatible revisions and invalid or mismatched canonical tile bytes before atomic cache persistence. Terrain acquisition follows the existing remote-worker/CPU-worker split, including the embedded library path.

For Step 2 raster requests, upstreams exchange `X-AOWIS-Raster-Source` (SHA-256 of the effective raster provider URL template), `X-AOWIS-Cascade-Visited` (comma-separated process UUIDs, maximum eight hops), and optional `X-API-Key`. A mismatched or missing response fingerprint is rejected. Redirects are not followed. Cascade-only instances never contact original raster providers.

## Raster provider discovery (Stage A)

With configured MAP upstreams, the server fetches each upstream's read-only
`GET /map/v1/providers/available` catalog (falling back to the older
`GET /map/v1/providers` when necessary). It merges local and inherited providers
**without modifying local AOWIS-CONFIG provider records**. Downstream clients
read the combined catalog from `/map/v1/providers/available`, where each entry
includes `origin: local|inherited` and the ordered immediate `upstreams` names.
The existing `/map/v1/providers` GET/PUT continues to manage **local** providers
only. Provider edits are never propagated upstream.

- A local definition (including a disabled one) takes precedence over an
  inherited provider with the same UUID. Upstream UUID collisions with
  different URL/style templates are not merged; the earlier upstream wins.
- Inherited raster providers are fetched only through the upstreams that
  advertise the matching source. They **never** fall back to a direct provider
  origin, even when global `allow_direct_fetch=true`. They still obey the
  downstream's raster remote-fetch and geographic coverage restrictions.
- Validated upstream catalog snapshots are stored under the MAP cache's
  `provider-catalogs/` directory. They survive restart and upstream outages.
  A failed refresh never replaces a last-known-good snapshot. No secret
  upstream API keys are written to these snapshots.
- Discovery is read-only and asynchronous, refreshing at startup and every 15
  minutes while upstreams are configured. It does not yet provide GUI-side
  inherited-provider toggles or per-provider routing controls (later stages).

## Wire representation on slow links

A downstream with `poor_internet_mode=true` requests optimized representations from each MAP upstream. For raster data it sends `X-AOWIS-Raster-Transfer: adaptive-jpeg;quality=75` (using configured quality) alongside a valid `X-AOWIS-Cascade-Visited` trace. The upstream ignores the compression request without a cascade trace. Responses vary on `X-AOWIS-Raster-Transfer`. Upstreams support this opt-in even if their own poor-internet mode is off. Normal GUI endpoints, tile addresses and public API behavior stay compatible.

**Raster imagery:** for an eligible *opaque PNG* tile, the upstream may respond with a libjpeg-turbo JPEG at the requested quality; its actual `Content-Type` must be `image/jpeg` even though the current XYZ URL ends in `.png`. Otherwise send the original bytes and MIME type. Never flatten transparency or re-encode an existing JPEG (including one received from another MAP server). Preserve existing WebP/JPEG unless a later measured, explicitly lossless-safe optimization is introduced. Honor upstream cache restrictions and `allow_lossy_transfer=false` in `[map_provider.UUID]` as a provider-specific hard opt-out. The field is also persisted by the provider-state API.

**Terrain:** losslessly zstd-compress the existing canonical `.aowterrain` bytes if the compressed representation is smaller; otherwise send them unchanged. Use negotiated HTTP content encoding: a MAP-to-MAP request must include both a nonempty, validated `X-AOWIS-Cascade-Visited` trace and `Accept-Encoding: zstd`; only then may the reply carry `Content-Encoding: zstd`. `Accept-Encoding` alone is insufficient because Qt GUI clients may send it automatically. Ordinary GUI responses remain uncompressed. This is **not** a new terrain tile format. The downstream decompresses with strict byte/size limits, validates dataset, XYZ, format/normalization revision and CRC, then atomically caches canonical bytes. Do not mix incompatible normalization revisions or silently accept a different terrain dataset.

The downstream must verify raster provider identity **and effective imagery source/style** before accepting an upstream raster tile; matching UUID alone is not sufficient if provider configurations differ. Keep upstream read authentication distinct from credentials exposed to ordinary GUI clients.

## Adaptive PNG → JPEG decision

Do not blindly encode every raster tile. Evaluate content in strata rather than imposing a single provider-wide decision. **Thresholds and behavior (motivated by Bitkine cache measurements; rerun to validate the new selector):**

| Input | Action |
| --- | --- |
| Existing JPEG/WebP; PNG with transparency; provider opt-out | Forward original, no lossy conversion |
| PNG below 12 KiB | Forward original |
| Opaque PNG 12–96 KiB | Encode the first eight candidates in each source/zoom/size bucket; continue if JPEG has won recently |
| Bucket with eight consecutive losing encodes | Probe one in 32 eligible tiles; a winning probe immediately restores full encoding |
| Opaque PNG above 96 KiB | Always try JPEG, subject to CPU limits |

Buckets are keyed by **provider/source style, zoom band and original byte-size band**, not just provider. The selector keeps the last eight actual encoding outcomes for each bucket. It attempts every eligible PNG until eight consecutive encodes fail the size threshold; only then does it back off to a probe every 32 visits. One successful probe resumes full evaluation. The HTTP server caps parallel encoders at two and sends original bytes immediately when busy. Thus a winning bucket does not arbitrarily skip 25% of its tiles, as the original predictor did in the Bitkine benchmark. Very large PNGs remain eligible even if their bucket generally loses.

For **every** JPEG actually encoded, compare its real size to the real original size. Select JPEG only if it saves at least **10% and 2 KiB** (initial thresholds); otherwise transmit the original. These are *size* decisions, not evidence of acceptable visual quality: keep transparency and provider opt-outs as hard restrictions. Do not use machine learning or image recognition.

Keep a bounded in-memory transfer-variant/decision cache keyed by original content SHA-256 and quality (currently 16 MiB / 4096 entries, reset on restart); never repeatedly decode/encode unchanged tiles. Upstream original caches remain unmodified. A downstream may retain a received JPEG for offline raster rendering and must not apply a further lossy generation at the next cascade hop. A changed source or quality invalidates its old decision. When transcoding workers are saturated, forward original bytes immediately instead of building an unbounded queue. Deduplicate concurrent encodes for the same content.

Expose counters for original/transmitted bytes, actual savings, selected JPEGs, skipped/sampled tiles, terrain zstd savings, encode CPU time, encoding-cache hits and upstream failover. Test with real imagery; savings estimates in discussions are *not* benchmarks.

## Deployment and Standalone

- **Systemd / ordinary server:** explicit administrator INI cascade settings override the MAP-owned AOWIS-CONFIG cascade record. No changes to the GUI's ordinary MAP connection contract. Servers may serve clients and downstream servers simultaneously.
- **Standalone:** an ordered list of upstream MAP URLs/keys, direct-fetch fallback and slow-link preferences is managed through GUI Map Settings and persisted by the MAP library through AOWIS-CONFIG. Standalone embeds `MapTiles` and `TerrainData` rather than running a second MAP HTTP server.
- Cached areas must continue rendering without WAN access. No automatic global prefetch or offline-package mechanism is part of this feature.

## Required acceptance tests

1. Two and three MAP instances: cold miss → prioritized upstream → local cache → offline cache hit; second upstream succeeds after first fails; direct origin contacted **only** when explicitly permitted.
2. Raster and terrain remote-fetch/coverage gates still work. A cyclic cascade fails promptly; upstream timeouts, invalid credentials, wrong provider identity, corrupted data and incompatible terrain revisions do not poison caches.
3. PNG versus JPEG: small tile, transparent tile, street map, satellite tile, pre-existing JPEG, quality change, variant reuse, saturated transcoding workers and no second JPEG generation across hops.
4. zstd versus identity terrain: decode/validate/store canonical bytes; corrupt/truncated/oversized payloads rejected; tile bytes match the non-cascaded result exactly.
5. Existing public GUI endpoints and no-upstream configuration remain backward compatible; standalone and systemd use the same library behavior.
6. Repeatable benchmark reports **wire bytes and CPU time** for mixed real raster tiles and Copernicus heightfields; no claim of performance improvement without measurements.

Step 5 implementation uses libpng for opaque-PNG detection/decoding and libjpeg-turbo for JPEG encoding. It admits at most two concurrent encoding jobs; saturated servers return original tiles without queuing. Variant selection applies to both warm-cache and completed async requests.

## Measure transfer savings on an actual cache

Build and run the read-only benchmark (no internet or cache modifications):

```bash
./compile_linux_tests.sh
./build-linux-tests/aowis-server-map-transfer-benchmark \
  --raster /path/to/cache/maptiles \
  --terrain /path/to/cache/terrain/normalized \
  --max-files 2000 > map-transfer-benchmark.json
```

Pass either directory separately if needed. The tool recursively reads actual
cached image files and canonical `.aowterrain` files. Its default adaptive JPEG
sampling matches the MAP selector; `--exhaustive-jpeg` instead evaluates every
eligible PNG for an upper-bound comparison. The report includes payload bytes,
selected JPEG/zstd counts, raster savings by provider and codec CPU time. It does **not** measure network
latency, HTTP framing, provider-specific lossless-only policy or concurrent
server load. Representative imagery and terrain from the actual project area
are needed before making real-world savings claims.

### Geographic benchmark: Bitkine, Chad

For a meaningful project-area comparison, sample cached tiles intersecting a
**25 km radius around Bitkine (11.9801° N, 18.2138° E)** instead of taking the
first files in lexical order. The following two commands use the *same* regional
sample, across available providers and zoom levels:

```bash
./build-linux-tests/aowis-server-map-transfer-benchmark \
  --raster "$HOME/.local/share/aowis-server-gui/maptiles" \
  --terrain "$HOME/.local/share/aowis-server-gui/terrain/normalized" \
  --region bitkine --radius-km 25 --min-zoom 8 --max-zoom 16 \
  --max-files 2000 > bitkine-adaptive.json

./build-linux-tests/aowis-server-map-transfer-benchmark \
  --raster "$HOME/.local/share/aowis-server-gui/maptiles" \
  --terrain "$HOME/.local/share/aowis-server-gui/terrain/normalized" \
  --region bitkine --radius-km 25 --min-zoom 8 --max-zoom 16 \
  --max-files 2000 --exhaustive-jpeg > bitkine-exhaustive.json
```

`region_coverage` reports candidate and sampled tiles, split by zoom and source.
The tool fails when a requested category has no cached tiles in the region;
**it never substitutes tiles from elsewhere or downloads new data**. Visit
Bitkine in the GUI first to populate the raster/3D terrain cache, with an
imagery provider whose terms permit such use. A sparse cache is not evidence
of representative savings. No tile imagery is fetched by the benchmark.

For the initial Bitkine 25 km sample (2,000 raster tiles, JPEG 75), the old
adaptive policy saved 25.24% versus 43.92% with exhaustive attempts. This
difference motivated the revised predictor; rerun both modes to verify it
reduces missed savings. Terrain zstd saved 2.57% on 210 cached tiles. These
measurements exclude provider-level lossy-transfer opt-outs, HTTP overhead and
actual WAN latency.
