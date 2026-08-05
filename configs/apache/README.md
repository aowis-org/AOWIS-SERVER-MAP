# Apache reverse-proxy configurations

## Debian package installation

The Debian package installs both templates directly into `/etc/apache2/sites-available/` and enables the required Apache modules. Neither site is enabled automatically because the templates are alternatives and the Internet-facing template requires deployment-specific values.

For a same-host installation, edit and enable:

```bash
sudoedit /etc/apache2/sites-available/aowis-map.localhost.conf
sudo a2ensite aowis-map.localhost.conf
```

For an Internet-facing proxy, edit and enable:

```bash
sudoedit /etc/apache2/sites-available/aowis-server-map.conf
sudo a2ensite aowis-server-map.conf
```

Then validate and reload Apache:

```bash
sudo apache2ctl configtest
sudo systemctl reload apache2
```

## Available configurations

Two separate templates are provided. They are alternatives; enable the one matching the deployment.

## Same-host HTTP setup

Use `aowis-map.localhost.conf` when the GUI-facing Apache server and `aowis-server-map` run on the same machine and HTTPS is not needed.

Edit these values first:

```apache
Define AOWIS_MAP_SERVER_NAME aowis-server-map.localhost
Define AOWIS_MAP_BACKEND http://127.0.0.1:8122
Define AOWIS_MAP_ALLOWED_ORIGIN http://aowis-server-gui.localhost
```

`AOWIS_MAP_ALLOWED_ORIGIN` must be the exact scheme, host and optional port from which the browser GUI is served.

Install it on Debian:

```bash
sudo a2enmod headers proxy proxy_http reqtimeout
sudo install -m 0644 configs/apache/aowis-map.localhost.conf /etc/apache2/sites-available/aowis-map.localhost.conf
sudo a2ensite aowis-map.localhost.conf
sudo apache2ctl configtest
sudo systemctl reload apache2
```

Add the selected local names to DNS or `/etc/hosts` as appropriate.

## Internet-facing HTTPS setup with Certbot

Use `aowis-server-map.conf` on the public reverse-proxy machine. The repository template intentionally contains only an initial port-80 virtual host: it can pass Apache configuration validation before a certificate exists. Certbot then obtains the Let's Encrypt certificate, creates/enables the HTTPS virtual host and changes HTTP to an HTTPS redirect.

The public template uses literal values rather than Apache `Define` variables so that Certbot can reliably discover and modify the virtual host. Replace all occurrences of these three examples before enabling the site:

```text
maps.example.com
http://127.0.0.1:8122
https://app.example.com
```

The backend may be on the same machine or directly on a private-network host. For a remote backend, replace both `ProxyPass` backend URLs, for example:

```apache
ProxyPass / http://10.20.0.15:8122/ connectiontimeout=5 timeout=65 retry=0
ProxyPassReverse / http://10.20.0.15:8122/
```

No reverse proxy is needed on the map-server host. Configure the map server to listen on its private address and allow TCP port 8122 only from the public proxy host:

```ini
[server]
listen_address=10.20.0.15
port=8122
```

Before requesting the certificate:

- Point the public DNS name at the Apache proxy.
- Permit inbound TCP ports 80 and 443.
- Keep the map-server backend itself off the public Internet.
- Set `AOWIS_MAP_ALLOWED_ORIGIN` to the exact HTTPS origin of the browser GUI.

Install the Apache site and Certbot on Debian:

```bash
sudo apt update
sudo apt install certbot python3-certbot-apache
sudo a2enmod headers proxy proxy_http reqtimeout rewrite ssl
sudo install -m 0644 configs/apache/aowis-server-map.conf /etc/apache2/sites-available/aowis-server-map.conf
sudo a2ensite aowis-server-map.conf
sudo apache2ctl configtest
sudo systemctl reload apache2
```

Obtain and install the certificate, and enable the HTTP-to-HTTPS redirect:

```bash
sudo certbot --apache --redirect -d maps.example.com
```

Replace `maps.example.com` with the literal `ServerName` configured in the installed virtual host. Certbot edits the installed Apache configuration under `/etc/apache2`; do not add certificate paths to the repository template before the certificate exists.

Verify renewal afterwards:

```bash
sudo certbot renew --dry-run
```

The proxy passes `Authorization` and `X-API-Key` to the backend. Use the normal API key for `/status` and tile GET requests, and the separate delete API key for cache DELETE requests.
