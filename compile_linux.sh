#!/bin/bash

cmake -S . -B build-linux
cmake --build build-linux

./build-linux/aowis-epanet-server
