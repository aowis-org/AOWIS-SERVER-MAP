# Apache reverse proxy for AOWIS Server Map

AOWIS Server Map listens on `127.0.0.1:8122` by default. Its Qt HTTP server
should be reached through a reverse proxy rather than exposed directly to the
public Internet. The supplied Apache templates are **alternatives**: select
one based on where the GUI, proxy and backend run.

| Template | Use case |
| --- | --- |
| `aowis-map.localhost.conf` | Same-host HTTP proxy for local GUI/development |
| `aowis-server-map.conf` | Internet-facing HTTPS proxy, bootstrapped with Certbot |

Both templates forward the map/terrain API (including provider catalog,
upstream activity and cache-management routes). Their CORS response headers
are intended for a configured browser GUI origin; **CORS is not authentication**.
In particular, the map-provider editing `PUT /map/v1/providers` route currently
has no API-key check. Only trusted clients should be able to reach it.

## Debian package installation

The Debian package places both templates under
`/etc/apache2/sites-available/` and enables the required Apache modules. It
does **not** enable either site. Administrators should edit the installed
site in place with `sudoedit`; do not blindly copy a repository template over
an existing customized site.

For the same-host configuration:

```bash
sudoedit /etc/apache2/sites-available/aowis-map.localhost.conf
sudo a2ensite aowis-map.localhost.conf
sudo apache2ctl configtest
sudo systemctl reload apache2
```

For an Internet-facing configuration, use
`/etc/apache2/sites-available/aowis-server-map.conf` and follow the Certbot
steps below.

## Same-host HTTP setup

`configs/apache/aowis-map.localhost.conf` is intended for Apache and the map
server on the same host, when HTTPS is not needed. Review these definitions:

```apache
Define AOWIS_MAP_SERVER_NAME aowis-server-map.localhost
Define AOWIS_MAP_BACKEND http://127.0.0.1:8122
Define AOWIS_MAP_ALLOWED_ORIGIN http://aowis-server-gui.localhost
```

The allowed origin is the **exact** browser GUI scheme, hostname and optional
port. Configure local DNS or `/etc/hosts` entries as appropriate.

For a manual deployment (without the Debian package), install the example
only if the destination does not already exist, then edit it:

```bash
sudo a2enmod headers proxy proxy_http reqtimeout
sudo install -m 0644 configs/apache/aowis-map.localhost.conf \
    /etc/apache2/sites-available/aowis-map.localhost.conf
sudoedit /etc/apache2/sites-available/aowis-map.localhost.conf
sudo a2ensite aowis-map.localhost.conf
sudo apache2ctl configtest
sudo systemctl reload apache2
```

Do not run the `install` step over an existing customized site without first
reviewing it. Keep the backend on loopback if Apache is on the same machine.

## Public HTTPS setup with Certbot

The `aowis-server-map.conf` template initially defines a port-80 virtual host
so Apache can validate and load it **before** a TLS certificate exists.
Replace all occurrences of the following placeholders:

```text
maps.example.com
http://127.0.0.1:8122
https://app.example.com
```

They represent the public map hostname, map-server backend, and allowed browser
GUI origin respectively. The public template uses literal values so Certbot
can discover and edit the Apache virtual host.

If the backend runs on another host, point the proxy at its *private* IP,
for example:

```apache
ProxyPass / http://10.20.0.15:8122/ connectiontimeout=5 timeout=65 retry=0
ProxyPassReverse / http://10.20.0.15:8122/
```

The map server on that host must bind to its private address:

```ini
[server]
listen_address=10.20.0.15
port=8122
```

Restrict TCP port 8122 to the proxy host at the backend firewall. No second
reverse proxy is required on the map-server machine.

Before requesting a certificate, ensure public DNS resolves to Apache and
ports 80 and 443 reach the proxy. Then:

```bash
sudo apt update
sudo apt install certbot python3-certbot-apache
sudo a2enmod headers proxy proxy_http reqtimeout rewrite ssl
sudoedit /etc/apache2/sites-available/aowis-server-map.conf
sudo a2ensite aowis-server-map.conf
sudo apache2ctl configtest
sudo systemctl reload apache2
sudo certbot --apache --redirect -d maps.example.com
sudo certbot renew --dry-run
```

Use the real configured `ServerName` in the Certbot command. Certbot installs
the HTTPS virtual host and HTTP-to-HTTPS redirect; administrators should
manage the resulting files under `/etc/apache2` as local configuration.

For a first-time **manual** install, copy the repository template to
`/etc/apache2/sites-available/aowis-server-map.conf` before `sudoedit`, but
do not overwrite an existing file. When using the Debian package the file
already exists.

The reverse proxy passes `Authorization` and `X-API-Key` headers through to the
backend. Read endpoints use `authentication/api_key`, while cache DELETE and
upstream cancellation use `authentication/delete_api_key`. The provider-edit
PUT endpoint is currently unauthenticated; network-level access restrictions
remain essential. See [the API reference](../../README.md#http-api).
