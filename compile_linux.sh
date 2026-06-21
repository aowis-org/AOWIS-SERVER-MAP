#!/bin/bash

cmake -S . -B build-linux
cmake --build build-linux

./build-linux/src/server/aowis-server-map
