#!/bin/bash
# Starts the demo image, asks it for its health, its page and one verdict, then stops and removes that container.
#
#   scripts/docker_smoke.sh [image]      (default four-eyes:demo; prefix with sudo if your docker needs it)
set -euo pipefail
IMAGE="${1:-four-eyes:demo}"
D="${DOCKER:-docker}"
cid=$($D run -d -p 127.0.0.1:8092:8092 "$IMAGE")
trap '$D rm -f "$cid" >/dev/null' EXIT
for _ in $(seq 1 30); do curl -fs http://127.0.0.1:8092/api/health >/dev/null && break; sleep 1; done
curl -fs http://127.0.0.1:8092/api/health; echo
curl -fs http://127.0.0.1:8092/ | grep -o '<title>[^<]*'
curl -fs -X POST -H 'Content-Type: application/json' -d '{"case":"s01"}' http://127.0.0.1:8092/api/request \
    | python3 -c 'import json,sys; e=json.load(sys.stdin)["entries"]; print([(x["signal"].split(".")[-2], x["verdict"], x["reason"]) for x in e if x["kind"]=="proposal"])'
echo "runs as: $($D exec "$cid" id -un); compiler in image: $($D exec "$cid" sh -c 'command -v gcc || echo none')"
