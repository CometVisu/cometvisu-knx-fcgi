#!/usr/bin/env bash
#
# fcgi-curl.sh — Send HTTP-style requests to the cometvisu-knx-fcgi backend
#                via FastCGI, using cgi-fcgi as the protocol bridge.
#
# Usage:
#   ./tools/fcgi-curl.sh GET  '/r?s=0&a=KNX:1/2/3&t=30'
#   ./tools/fcgi-curl.sh POST '/w' 's=0&a=10/20&v=800c6f'
#   ./tools/fcgi-curl.sh POST '/l' 'u=admin&p=secret'
#
# Environment variables:
#   FCGI_PORT   TCP port the backend listens on (default: 9000)
#   FCGI_HOST   Host where the backend runs     (default: 127.0.0.1)
#
# Examples (once the backend is running via "Run against real knxd"):
#
#   # Login (creates a session; the anonymous id is "0")
#   ./tools/fcgi-curl.sh POST '/l' 'u=admin&p=admin'
#
#   # Read an address from the cache (s= is mandatory, "0" = anonymous)
#   ./tools/fcgi-curl.sh GET '/r?s=0&a=KNX:1/2/3&t=5'
#
#   # Write a value (v carries the APCI byte, "80" + value)
#   ./tools/fcgi-curl.sh POST '/w' 's=0&a=10/20&v=800c6f'
#
#   # Read with long-poll (waits up to 30 seconds for a telegram)
#   ./tools/fcgi-curl.sh GET '/r?s=0&a=KNX:1/2/3&t=30'

set -euo pipefail

PORT="${FCGI_PORT:-9000}"
HOST="${FCGI_HOST:-127.0.0.1}"
METHOD="${1:-GET}"
TARGET="${2:-/}"
BODY="${3:-}"

# Split target into path and query string
PATH_PART="${TARGET%%\?*}"
QUERY_PART="${TARGET#*\?}"
if [ "$QUERY_PART" = "$TARGET" ]; then
  QUERY_PART=""
fi

# For GET/HEAD requests, merge body into the query string (the body is
# sent via stdin to cgi-fcgi, but FCGI only passes QUERY_STRING to the
# application for GET — the body is effectively discarded).
if [ "$METHOD" = "GET" ] || [ "$METHOD" = "HEAD" ]; then
  if [ -n "$BODY" ] && [ -n "$QUERY_PART" ]; then
    QUERY_PART="${QUERY_PART}&${BODY}"
  elif [ -n "$BODY" ]; then
    QUERY_PART="$BODY"
  fi
  BODY=""
fi

CONTENT_LENGTH="${#BODY}"

export REQUEST_METHOD="$METHOD"
export SCRIPT_NAME="$PATH_PART"
export REQUEST_URI="$TARGET"
export QUERY_STRING="$QUERY_PART"
export SERVER_NAME="localhost"
export SERVER_PORT="80"
export SERVER_PROTOCOL="HTTP/1.1"
export CONTENT_TYPE="application/x-www-form-urlencoded; charset=utf-8"
export CONTENT_LENGTH="$CONTENT_LENGTH"

# Print the request summary to stderr so the user can see what's happening
echo "[fcgi-curl] $METHOD $TARGET" >&2
if [ -n "$BODY" ]; then
  echo "[fcgi-curl] Body: $BODY" >&2
fi
echo "[fcgi-curl] → ${HOST}:${PORT}" >&2
echo "---" >&2

# cgi-fcgi reads the request body from stdin
# -bind -connect is required for TCP connections (host:port), otherwise
# cgi-fcgi expects an app path as an additional argument.
echo -n "$BODY" | cgi-fcgi -bind -connect "${HOST}:${PORT}"
