# AOWIS-Map-Server
RESTful Caching Map Tile Server for **AOWIS-EPANET-GUI**

**IMPORTANT:**

This Server uses QHttpServer, which is **NOT suitable for exposure to the public Internet**! It does **NOT offer HTTPS**, either.

Usually, this Software runs on the same device as **AOWIS-EPANET-GUI**, and the **port MUST be closed by the firewall**.
If you decide to set it up on different machines, create a proper VPN connection between them to get TLS!

## Build
### Linux
Run the script `compile_linux.sh`

Find the result in the folder `build-linux`

### Windows / macOS
At this point, Windows and macOS are not officially supported by AOWIS. However, you can build this application for them, just as any other Qt-Application.


