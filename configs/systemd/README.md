# Debian 13 systemd setup

The system service uses:

- Binary: `/usr/bin/aowis-server-map`
- Configuration: `/etc/aowis-server-map/map-server.ini`
- Tile cache: `/var/cache/aowis-server-map`
- Service account: `aowis-server-map`

The unit uses `CacheDirectory=aowis-server-map`. systemd creates the cache directory with the correct ownership and exports `CACHE_DIRECTORY=/var/cache/aowis-server-map` to the process.

The unit also passes `--require-api-key` and `--require-delete-api-key`, so it refuses to start unless both keys are configured.

## Automated installation

Build the server, then run the installer from the project root:

```bash
sudo configs/systemd/install.sh
```

By default, it installs `build-linux/aowis-server-map`. A different binary can be selected explicitly:

```bash
sudo configs/systemd/install.sh --binary /path/to/aowis-server-map
```

On the first installation, the installer:

- Creates the dedicated system group and user when missing.
- Installs `/usr/bin/aowis-server-map`.
- Creates `/etc/aowis-server-map/map-server.ini`.
- Generates separate random 256-bit read and delete API keys.
- Writes the generated keys into the configuration.
- Prints both keys once in the terminal for the administrator.
- Installs the systemd unit and reloads systemd.

On later runs, the installer updates the binary and unit but preserves the existing configuration and API keys unchanged.

Install and start immediately:

```bash
sudo configs/systemd/install.sh --enable-now
```

Or install, edit, and start separately:

```bash
sudo configs/systemd/install.sh
sudoedit /etc/aowis-server-map/map-server.ini
sudo systemctl enable --now aowis-server-map.service
```

## Local and remote reverse proxies

When Apache runs on the same host, keep:

```ini
listen_address=127.0.0.1
```

When the reverse proxy runs on another private-network host, bind the map server to its private address:

```ini
listen_address=10.20.0.15
```

Then allow TCP port 8122 only from the reverse-proxy host using the server firewall. No second reverse proxy is needed on the map-server host.

## Administration

```bash
sudo systemctl status aowis-server-map.service
sudo journalctl -u aowis-server-map.service -f
sudo systemctl restart aowis-server-map.service
```

The normal API key authorizes `/status` and tile GET requests. The separate delete API key authorizes cache DELETE requests. Both use either:

```http
X-API-Key: the-endpoint-specific-key
```

or:

```http
Authorization: Bearer the-endpoint-specific-key
```

## Configuration upgrades

Re-running `install.sh` never replaces `/etc/aowis-server-map/map-server.ini`. When new configuration keys are introduced, compare the installed file with `configs/systemd/map-server.ini` and add the desired keys manually.
