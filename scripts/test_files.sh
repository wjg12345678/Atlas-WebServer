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

TMP_FILE="$(make_demo_file)"
trap 'rm -f "$TMP_FILE"' EXIT INT TERM
CONTENT_BASE64="$(file_to_base64 "$TMP_FILE")"

UPLOAD_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/private/files" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"filename\":\"demo.txt\",\"content_base64\":\"$CONTENT_BASE64\",\"content_type\":\"text/plain\"}")"
FILE_ID="$(extract_file_id "$UPLOAD_RESPONSE")"
if [ -z "$FILE_ID" ]; then
    echo "upload failed: $UPLOAD_RESPONSE"
    exit 1
fi

echo "login: $LOGIN_RESPONSE"
echo "upload: $UPLOAD_RESPONSE"
echo "list: $(curl -sS "$BASE_URL/api/private/files" -H "Authorization: Bearer $TOKEN")"
echo "download headers/body:"
curl -i -sS "$BASE_URL/api/private/files/$FILE_ID/download" -H "Authorization: Bearer $TOKEN"
echo
echo "delete: $(curl -sS -X DELETE "$BASE_URL/api/private/files/$FILE_ID" -H "Authorization: Bearer $TOKEN")"
echo "logout: $(curl -sS -X POST "$BASE_URL/api/private/logout" -H "Authorization: Bearer $TOKEN")"
