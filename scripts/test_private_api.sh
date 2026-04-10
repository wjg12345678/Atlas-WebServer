#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

wait_for_server
register_user

LOGIN_INFO="$(require_token)"
LOGIN_RESPONSE="$(printf '%s\n' "$LOGIN_INFO" | sed -n '1p')"
TOKEN="$(printf '%s\n' "$LOGIN_INFO" | sed -n '2p')"

echo "login: $LOGIN_RESPONSE"
echo "private ping: $(curl -sS "$BASE_URL/api/private/ping" -H "Authorization: Bearer $TOKEN")"
echo "operations: $(curl -sS "$BASE_URL/api/private/operations" -H "Authorization: Bearer $TOKEN")"
echo "logout: $(curl -sS -X POST "$BASE_URL/api/private/logout" -H "Authorization: Bearer $TOKEN")"
