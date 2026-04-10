#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

wait_for_server
register_user

LOGIN_RESPONSE="$(login_user)"
TOKEN="$(extract_token "$LOGIN_RESPONSE")"
if [ -z "$TOKEN" ]; then
    echo "login failed: $LOGIN_RESPONSE"
    exit 1
fi

echo "login: $LOGIN_RESPONSE"
echo "private ping: $(curl -sS "$BASE_URL/api/private/ping" -H "Authorization: Bearer $TOKEN")"
echo "logout: $(curl -sS -X POST "$BASE_URL/api/private/logout" -H "Authorization: Bearer $TOKEN")"
