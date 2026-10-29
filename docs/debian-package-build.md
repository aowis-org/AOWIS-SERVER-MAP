# Debian packages: Docker build and installation qualification

AOWIS Server Map uses Debian's normal `dpkg-buildpackage` / `debhelper` packaging
machinery, defined in `debian/`. The native `compile_deb.sh` script is still the
package builder. `compile_deb_docker.sh` runs it in a controlled **Debian 13
(trixie)** build container, so building a `.deb` does not require modifying the
host's packages or changing the Linux development environment.

## Prerequisites

Docker with an accessible daemon and a working network connection for the
initial Debian image/package downloads. Building does not require root on the
host if the current account already has permission to use the Docker daemon.
Docker daemon access itself is highly privileged; use only a trusted Docker
installation and trusted project sources.

### Docker DNS troubleshooting

If the Debian base image downloads successfully but `apt-get update` reports
`Temporary failure resolving 'deb.debian.org'`, the failure is Docker's
container DNS/network path, **not** a C++ build or missing Debian packages.
On Linux, try the existing host networking stack for the *image build*:

```bash
./compile_deb_docker.sh --build-network host
```

The default remains Docker bridge networking. The `--build-network` option
changes networking **only while installing the toolchain inside the Docker
image**; it does not change the network of the installed service on your host.
It relies on the host having working Debian repository DNS and HTTP access.

For the optional installation qualification, the test container has its own
network setting. If it also fails to resolve Debian package repositories:

```bash
./compile_deb_docker.sh --test --build-network host --test-network host
```

**Caution:** `--test-network host` shares the host's network namespace. The
qualification server binds **127.0.0.1:18122 on the host** for the duration
of the test. Use this only on a trusted Linux development machine with that
port free; the default bridge network keeps it isolated. The test still uses
its own disposable filesystem, users and processes and does not modify the
host's map service on port 8122. Fixing Docker daemon DNS is preferable for
repeatable isolated tests in CI. No `--network host` mode is silently enabled.

A Docker warning about the deprecated legacy image builder is unrelated to
Debian DNS resolution. Installing the Docker buildx plugin removes that
warning but is not required to resolve this failure.

## Build a package

From the project root:

```bash
./compile_deb_docker.sh
```

For Linux hosts whose Docker bridge cannot resolve Debian repositories, use
`--build-network host` as described above.

To build a different Debian package revision:

```bash
./compile_deb_docker.sh --revision 3
```

The wrapper builds a tagged Debian 13 toolchain image from
`tools/debian/Dockerfile`, mounts the checked-out source read-only, copies it
into a disposable build directory without local build trees, and invokes the
existing `compile_deb.sh` script. It writes a binary package to `dist/` using
the **host user's UID/GID**. No Debian dependencies are installed on the host,
and the wrapper does not start, stop, or replace any host service.

`compile_deb.sh --install-dependencies` is still available for native Debian
builds without Docker.

## Qualify an installed package

```bash
./compile_deb_docker.sh --test
```

The `--test` workflow builds the selected Debian package revision and one
higher revision. It then launches a fresh, disposable Debian 13 container and
checks actual installation and package-manager lifecycle behavior:

1. Install the package with `apt`, including declared runtime dependencies.
2. Verify the binary, service unit, Apache templates and dedicated user/group.
3. Check the installed configuration and distinct random API keys.
4. Validate the vendor unit with `systemd-analyze verify`.
5. Start the **installed** binary as its unprivileged service account, on
   container-local port **18122**, with the installed INI and required API keys.
6. Verify authenticated status, provider-list, and terrain-dataset HTTP reads;
   verify that unauthenticated status is denied.
7. Upgrade to the next package revision and verify that `/etc` configuration,
   permissions, `/var/lib` provider state, and `/var/cache` contents survive.
8. Remove/reinstall the package and check preservation again, then purge and
   verify the package-owned administrator INI is removed while service data
   remains untouched.

These are **package installation and runtime HTTP smoke tests**. A normal
Docker container does not run systemd as PID 1, so the test deliberately
blocks automatic service starts during `apt` transactions. It does **not**
claim to test `systemctl start`, service restart policies, or a live Apache
reverse proxy. Those should be tested separately in a disposable Debian VM
booted with systemd, before production deployment.

The test uses no public tile sources and does not need to download a Copernicus
COG. It never binds port 8122 on the host or changes the installed host service.
Docker output includes temporary test-container package-installation keys;
those are discarded with the container.

## Package layout and administration

The package installs the binary at `/usr/bin/aowis-server-map`, a vendor unit
under `/usr/lib/systemd/system/`, an example INI under
`/usr/share/aowis-server-map/`, and disabled Apache site templates.
The package's first-install script generates `/etc/aowis-server-map/map-server.ini`
only if it does not exist. It creates the `aowis-server-map` service identity
and preserves administrator configuration and state during upgrades.

For actual service operation, runtime configuration ownership, and reverse
proxy setup, see [systemd deployment](../configs/systemd/README.md),
[Apache deployment](../configs/apache/README.md), and
[configuration](configuration.md).
