# Cascading MAP servers

An AOWIS MAP server can obtain **raster imagery and normalized 3D terrain** from
other MAP servers, retain the results locally and serve nearby GUI clients from
its cache. This is intended for branch offices and project sites with slow or
unreliable internet. Every MAP server can be both an upstream and a downstream.

```text
OSM / imagery sources + Copernicus GLO-30
                 |
        central MAP (upstream)
                 |  compressed MAP-to-MAP transfer
        site MAP (local cache)
                 |
          AOWIS GUI clients
```

Cascading transfers raster XYZ tiles and **canonical `.aowterrain` tiles**, not
entire Copernicus GeoTIFFs. No automatic regional prefetch or offline-package
mounting is provided. See [Configuration](configuration.md) for setting ownership
and [HTTP API](http-api.md) for all endpoints.

## Configure a site server

The central MAP server needs to be reachable and to permit the relevant
provider/dataset and downloads. On the **site** MAP server, add the following to
its administrator INI (`/etc/aowis-server-map/map-server.ini` for systemd):

```ini
[upstream]
servers=primary,backup
allow_direct_fetch=false

[upstream.primary]
url=https://maps-primary.example.org
api_key=PRIMARY_READ_KEY

[upstream.backup]
url=https://maps-backup.example.org
api_key=BACKUP_READ_KEY

[bandwidth]
poor_internet_mode=true
adaptive_compression=true
jpeg_quality=75
```

`servers` sets **failover order**. Names are unique identifiers, not hostnames;
each entry has its own MAP URL and **upstream read key**. Omit a redundant
upstream if you only have one. Use HTTPS or a protected private tunnel across
the WAN. Do not place keys in a URL or publish this file.

`allow_direct_fetch=false` prevents **all original-provider downloads** on a
cache miss (raster imagery and Copernicus), even when an upstream is unavailable.
With `true` (the default), a locally defined raster provider may use its
original source after MAP upstreams fail, subject to its acquisition mode;
terrain may download from Copernicus after upstream failure. An inherited raster
provider **never** downloads from its original imagery provider.

Restart a separately running server after **INI** changes:

```bash
sudo systemctl restart aowis-server-map
# Or, when running manually:
./build-linux/aowis-server-map --config ./map-server.ini
```

A manually started server can use an optional INI; Standalone has **no MAP INI**.
In AOWIS Standalone, use **Settings → Map Settings** to configure the ordered
upstream list, direct-fetch policy and slow-link settings. Changes are autosaved
through the MAP library's AOWIS-CONFIG repository and applied live to the
embedded MAP engine (`MapTiles::setCascadeConfig`, `TerrainData::setCascadeConfig`)
and provider discovery; no separate MAP daemon or GUI restart is required.
Later requests use the new upstreams, read keys, order and options; downloads
in flight from a changed upstream set are canceled and retried, upstream
cooldowns are reset, and cached tiles stay valid. A setting that fails
validation is not saved and the previous configuration stays in use. On a separate
server the public REST API does **not** edit the global upstream list: use the
administrator INI (or MAP-managed CONFIG records if not overridden by INI).

### Upstreams reached through WireGuard

In Standalone each upstream row has a **Connection** choice: *Direct
connection* or one of the central WireGuard profiles of **Settings →
WireGuard VPN** (the same profiles DVMDB and the GUI's MAP server connection
use). The MAP cascade record stores only the profile's ID, in the upstream's
`transport` object (`{"type":"wireguard","profile_id":"…"}`); keys and
WireGuard configuration never enter MAP settings. A record with such an
upstream is written as `version` 2, so an older MAP release rejects it rather
than contacting the upstream directly; unknown transport types, and a
WireGuard transport without a profile, make the record invalid (fail
closed).

MAP never contacts a WireGuard upstream directly. Its host sets the
upstream's runtime `proxy` (never persisted) to the local SOCKS5 proxy of
that profile's running tunnel; raster tiles, terrain and provider discovery
then reach the upstream only through it (one network manager per tunnel,
host names resolved inside the tunnel). Without a proxy the upstream is
skipped: other permitted upstreams are used in order and offline catalog
snapshots stay available. A changed proxy (tunnel restarted, profile
changed) cancels downloads in flight like any other upstream change.

Standalone resolves the profiles (`MapUpstreamTunnels`): one lease of each
profile's shared tunnel, so upstreams, the GUI's MAP server connection and
DVMDB connections using the same profile share one tunnel, retried with
backoff while it cannot start. A separately running MAP server has no
WireGuard transport, so such upstreams are always skipped there.

## Where a tile comes from

1. **Local cache first.** Existing raster and normalized-terrain tiles remain
   available when the WAN is down. Disabled raster providers cannot serve even
   their cached tiles.
2. **Permitted upstream MAP servers.** On a miss, contact the selected servers
   in order, trying the next if one fails. Raster provider discovery identifies
   which upstreams advertise a compatible provider/source.
3. **Original provider, only when permitted.** Local raster providers use their
   per-provider acquisition mode; terrain uses the global `allow_direct_fetch`
   switch. An inherited raster provider never takes this step.

Both `downloads/remote_fetch_enabled` and `terrain/remote_fetch_enabled`, along
with geographic coverage and per-provider restrictions, still gate **new remote
requests**. A cascade cannot override them. Cycle detection uses a propagated
visited-server trace and an eight-hop limit; provider-source fingerprints and
terrain format/revision checks prevent mismatched data entering caches.

### Provider inheritance and per-provider routing

`GET /map/v1/providers/available` merges local and upstream-advertised raster
providers. Local definitions win when UUIDs overlap; matching upstreams remain
visible as routes. Inherited definitions are **read-only**: editing the local
provider catalog never changes upstream providers.

On the downstream you can independently:

- Disable/re-enable an inherited provider without deleting its definition.
- Select an **ordered subset** of upstreams advertising a provider (or reset
  that provider to the global order).
- Set a raster provider's acquisition mode:

| Mode | Cache miss | Default / restriction |
| --- | --- | --- |
| **Automatic** | Selected MAP upstreams, then original source **only for local providers** when permitted | Local provider default |
| **Cascade** | Selected MAP upstreams only | Inherited provider default |
| **Direct** | Original imagery source only | Local provider only; requires global direct-fetch permission |

These overrides are persisted **on the downstream** in MAP-owned AOWIS-CONFIG,
not forwarded to upstream servers. Administrator download restrictions always
win. Routing and acquisition changes apply immediately via authenticated
`PUT /map/v1/providers/upstreams` and `PUT /map/v1/providers/acquisition`.
The GUI presents these settings in **Map Settings**; full REST examples are in
[HTTP API — Provider routing](http-api.md#provider-routing-and-acquisition).

A validated provider catalog snapshot survives failed discovery and restarts.
When the upstream is offline, cached tiles still render, but new uncached areas
cannot load unless a permitted alternative exists. **Do not confuse cached
coverage with a complete offline map.**

## Slow-link transfer

`poor_internet_mode=true` on the **downstream** requests optimized representations
from upstream MAP servers. The upstream does not need to enable this mode to
honor a valid cascade request. Public GUI responses remain compatible.

| Data | MAP-to-MAP transfer | Local storage |
| --- | --- | --- |
| Opaque PNG imagery | Adaptive libjpeg-turbo JPEG at quality 75 when worthwhile | Received image can be cached; avoid repeated lossy encoding across hops |
| Already JPEG/WebP, transparent PNG, provider lossless-only | Original bytes; no lossy conversion | Original representation |
| `.aowterrain` | zstd HTTP content encoding only if smaller | Decompressed, validated canonical terrain tile |

Raster JPEG conversion is **not universal**: PNGs below 12 KiB are left alone;
for larger opaque PNGs the selector compares actual bytes and requires at least
10% **and** 2 KiB saved. Decisions use recent results per source/style, zoom and
size band; losing buckets are probed periodically. Conversion uses at most two
concurrent workers; when busy, the upstream sends the original tile. Cached
originals are not rewritten. Provider `allow_lossy_transfer=false` is a hard
opt-out; imagery-provider licensing still applies.

A MAP raster request uses `X-AOWIS-Cascade-Visited` plus
`X-AOWIS-Raster-Transfer: adaptive-jpeg;quality=75`. The response's actual
`Content-Type` (possibly `image/jpeg` despite a `.png` URL) is authoritative.
Normalized terrain uses the cascade trace plus `Accept-Encoding: zstd`; the
response may have `Content-Encoding: zstd`. Ordinary GUI requests do not receive
this MAP-only zstd encoding. Downstreams validate both kinds before caching.

## Verify and troubleshoot

```bash
# On the site MAP server (supply its read key when configured):
curl -fsS -H 'X-API-Key: SITE_READ_KEY' \
  http://127.0.0.1:8122/map/v1/providers/available
curl -fsS -H 'X-API-Key: SITE_READ_KEY' \
  http://127.0.0.1:8122/upstream/v1/activity

# Run the MAP tests, including cascading, source validation and transfer codecs:
./compile_linux_tests.sh
```

If an inherited layer does not appear, check the **upstream read key**, the
upstream's `/map/v1/providers/available`, and whether the provider is enabled
and remote-fetchable there. If the catalog appears but tiles are missing, check
local download/coverage restrictions, the selected upstream names, provider
fingerprints, and cached versus uncached locations. Cached data should still be
available after disconnecting the upstream. A local `Direct` provider is the
only raster case that deliberately bypasses MAP peers.

### Measure bandwidth savings in the project area

The benchmark reads existing caches and **does not download tiles**. For the
Bitkine, Chad test area (25 km radius, zooms 8–16):

```bash
./build-linux-tests/aowis-server-map-transfer-benchmark \
  --raster "$HOME/.local/share/aowis-server-gui/maptiles" \
  --terrain "$HOME/.local/share/aowis-server-gui/terrain/normalized" \
  --region bitkine --radius-km 25 --min-zoom 8 --max-zoom 16 \
  --max-files 2000 > bitkine-adaptive.json
```

Add `--exhaustive-jpeg` to compare against encoding every eligible PNG.
The report separates original/selected payload bytes, codec time, source and
zoom coverage. These are **payload-size measurements**, not WAN throughput or
latency measurements; a cache must actually contain tiles from the area.
