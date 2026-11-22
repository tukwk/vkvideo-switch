#!/bin/sh
# Refresh the Mozilla CA bundle embedded into the NRO.
# Source: https://curl.se/ca/cacert.pem (Mozilla CA store, MPL 2.0).
set -e
cd "$(dirname "$0")/.."
curl -fsSL https://curl.se/ca/cacert.pem -o data/cacert.bin
echo "data/cacert.bin updated: $(grep -c 'BEGIN CERTIFICATE' data/cacert.bin) certificates"
