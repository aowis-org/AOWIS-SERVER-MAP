# Debian 13 systemd setup

The system service uses:

- Binary: `/usr/bin/aowis-server-map`
- Configuration: `/etc/aowis-server-map/map-server.ini`
- Tile cache: `/var/cache/aowis-server-map`
- Service account: `aowis-server-map`

The unit uses `CacheDirectory=aowis-server-map`. systemd creates the cache directory with the correct ownership and exports `CACHE_DIRECTORY=/var/cache/aowis-server-map` to the process.

## Installation

```bash
sudo install -m 0755 build-linux/aowis-server-map /usr/bin/aowis-server-map
sudo useradd --system --no-create-home --shell /usr/sbin/nologin aowis-server-map
sudo install -d -o root -g aowis-server-map -m 0750 /etc/aowis-server-map
sudo /usr/bin/aowis-server-map --write-default-config /etc/aowis-server-map/map-server.ini
sudo chown root:aowis-server-map /etc/aowis-server-map/map-server.ini
sudo chmod 0640 /etc/aowis-server-map/map-server.ini
sudo install -m 0644 tools/systemd/aowis-server-map.service /etc/systemd/system/aowis-server-map.service
sudo systemctl daemon-reload
sudo systemctl enable --now aowis-server-map.service
```

If the service account already exists, omit the `useradd` command.

## Administration

```bash
sudo systemctl status aowis-server-map.service
sudo journalctl -u aowis-server-map.service -f
sudo systemctl restart aowis-server-map.service
```

The service deliberately passes the `/etc` configuration path with `--config`. A normal user launch without `--config` instead checks `~/.local/share/aowis-server-map/map-server.ini` and uses `~/.local/share/aowis-server-map` for the cache.
