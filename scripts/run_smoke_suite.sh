#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "[1/3] auth"
"$SCRIPT_DIR/test_auth.sh"
echo

echo "[2/3] private-api"
"$SCRIPT_DIR/test_private_api.sh"
echo

echo "[3/3] files"
"$SCRIPT_DIR/test_files.sh"
