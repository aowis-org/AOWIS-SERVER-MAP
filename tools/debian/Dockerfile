# Build the Debian package with the target Debian release's dependencies.
# The source is mounted read-only at runtime; it is not copied into the image.
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install --yes --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      debhelper \
      dpkg-dev \
      g++ \
      libtiff-dev \
      ninja-build \
      pkg-config \
      qt6-base-dev \
      qt6-httpserver-dev \
      qt6-websockets-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
