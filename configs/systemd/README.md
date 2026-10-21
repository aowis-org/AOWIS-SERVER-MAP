# Linux systemd deployment

The system service runs `aowis-server-map` as a dedicated unprivileged account.
Its administrator-owned configuration is under `/etc`, API-managed provider
state under `/var/lib`, and disposable caches under `/var/cache`. The running
server never needs to write to `/etc`.

## Files and ownership

| Path | Purpose |
| --- | --- |
| `/usr/bin/aowis-server-map` | Installed executable |
| `/usr/share/aowis-server-map/map-server.ini` | Packaged example INI (Debian package) |
| `/etc/aowis-server-map/map-server.ini` | Administrator INI and API keys |
| `/var/lib/aowis-server-map/providers.ini` | Optional catalog written by GUI/API provider editor |
| `/var/lib/aowis-server-map/data/` | Permanent administrator-supplied data, if configured |
| `/var/cache/aowis-server-map/maptiles/` | Downloaded raster tiles |
| `/var/cache/aowis-server-map/terrain/` | Downloaded native and normalized elevation data |

The service unit sets `StateDirectory=aowis-server-map`,
`CacheDirectory=aowis-server-map`, `UMask=0027` and `ProtectSystem=strict`.
systemd creates the writable state/cache directories for the service user and
exports `STATE_DIRECTORY` and `CACHE_DIRECTORY`. The configuration uses these
defaults when the corresponding paths are empty.

The unit explicitly starts with:

```text
--config /etc/aowis-server-map/map-server.ini
--require-api-key
--require-delete-api-key
```

Both read and delete API keys must be configured for the packaged service.

## Debian package installation

From the project root:

```bash
./compile_deb.sh --install-dependencies
sudo apt install ./dist/aowis-server-map_<version>_<architecture>.deb
```

The package installs the vendor unit under `/usr/lib/systemd/system` and a
sample INI under `/usr/share`. It creates an administrator configuration with
two independent random API keys only when no configuration exists. It preserves
existing `/etc/aowis-server-map/map-server.ini` contents and permissions on
subsequent package operations. The service account is `aowis-server-map`.

The package also installs **disabled** Apache site templates; enabling one is
an explicit administrator choice. See [Apache setup](../apache/README.md).

## Manual installation (without Debian package)

Build the server, then:

```bash
./compile_linux.sh --no-run
sudo configs/systemd/install.sh
```

To start/enroll the service during installation:

```bash
sudo configs/systemd/install.sh --enable-now
```

To install a binary from a different location:

```bash
sudo configs/systemd/install.sh --binary /path/to/aowis-server-map
```

The installer creates the service account, installs the binary as
`/usr/bin/aowis-server-map`, installs the vendor unit at
`/usr/local/lib/systemd/system/aowis-server-map.service`, and prepares a fresh
`/etc/aowis-server-map/map-server.ini` with random read/delete keys only if
missing. It prints newly generated keys once. Re-running it updates the binary
and vendor unit without rewriting an existing `/etc` configuration, keys,
permissions or administrator-owned service overrides.

If the service was not started during installation:

```bash
sudoedit /etc/aowis-server-map/map-server.ini
sudo systemctl enable --now aowis-server-map.service
```

## Service administration

```bash
sudo systemctl status aowis-server-map.service
sudo journalctl -u aowis-server-map.service -f
sudo systemctl restart aowis-server-map.service
sudo systemctl cat aowis-server-map.service
```

Make unit customizations with:

```bash
sudo systemctl edit aowis-server-map.service
```

This creates an administrator drop-in under `/etc/systemd/system` rather than
editing a packaged vendor unit. **Any complete unit already present in
`/etc/systemd/system/aowis-server-map.service` takes precedence over vendor
units**. The manual installer deliberately does not remove or overwrite it.
Use `systemctl cat` to inspect which unit is active. If switching to a vendor
unit, preserve needed customizations in drop-ins and manage the `/etc` unit
explicitly; then run `sudo systemctl daemon-reload` and restart.

### Configuration management

`/etc/aowis-server-map/map-server.ini` is not regenerated on upgrade or server
startup. Compare installed settings to the example or print the current built-in
reference without altering files:

```bash
aowis-server-map --print-default-config
```

Check the **service configuration** (including the API-managed provider-state
file), without starting a listener:

```bash
sudo aowis-server-map --check-config --config /etc/aowis-server-map/map-server.ini
```

GUI provider editing saves **only** raster provider definitions to
`/var/lib/aowis-server-map/providers.ini`. If present, that catalog replaces
the provider list in `/etc` but does not change the configured port, keys,
regional download limits, terrain settings or storage directories. To resume
using `/etc`'s raster-provider sections, stop the service, back up and remove
`providers.ini`, then start the service. The file is otherwise persistent
through restarts and upgrades.

For office projects in multiple places, the administrator can define several
`[region.<id>]` bounding rectangles, one `[coverage.<id>]` with progressively
narrower `zoom_N` tiers, and separate `downloads/coverage` and
`terrain/coverage` choices. **Disabling raster remote downloads never disables
terrain remote downloads and vice versa**. Both remain able to serve their
existing local data. See [root README](../../README.md#geographic-download-coverage)
for a complete configuration example.

## Local and remote reverse proxies

Keep `server/listen_address=127.0.0.1` if Apache runs on the same host.
When a reverse proxy resides on another machine, bind the server to a private
address in its INI:

```ini
[server]
listen_address=10.20.0.15
port=8122
```

Allow TCP port 8122 **only from the reverse proxy** in the host firewall. The
proxy should terminate HTTPS. The provider-editing `PUT /map/v1/providers`
endpoint currently has no API authentication: don't expose it to untrusted
clients. Neither a reverse proxy nor CORS response headers alone authorize
this operation.

See [Apache reverse proxy configurations](../apache/README.md) for templates.
