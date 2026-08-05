# AOWIS-SERVER-MAP
RESTful Caching Map Tile Server for [AOWIS-SERVER-GUI](https://github.com/aowis-org/AOWIS-SERVER-GUI).

**IMPORTANT:**

This server uses QHttpServer, which is **not suitable for exposure to the public Internet**. It does not provide HTTPS by itself.

By default, the server listens only on `127.0.0.1:8122`. Keep the port closed by the firewall when changing the listen address. If the GUI and map server run on different machines, use a properly secured VPN or a TLS reverse proxy.

## Build
### Linux
Run the script `compile_linux.sh`.

The result is created in `build-linux`.

### Windows / macOS
Windows and macOS are not officially supported at this point. The project can nevertheless be built like a regular Qt application.

## Server options

```text
-a, --listen-address <address>       Listen IP address. Default: 127.0.0.1
-p, --port <port>                    Listen port. Default: 8122
--max-pending-requests <count>       Maximum pending tile HTTP requests. Default: 2048
--max-active-downloads <count>       Maximum simultaneous upstream downloads. Default: 32
```

Example for listening on all IPv4 interfaces:

```bash
./aowis-server-map --listen-address 0.0.0.0 --port 8122
```
