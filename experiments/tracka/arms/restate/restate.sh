#!/bin/sh
set -eu
cd "$(dirname "$0")"
p=$TRACKA_PORTS
export RESTATE_BASE_DIR=$TRACKA_RUN/restate RESTATE_NODE_NAME=tracka RESTATE_LISTEN_MODE=tcp \
  RESTATE_INGRESS__BIND_ADDRESS=127.0.0.1:$p RESTATE_ADMIN__BIND_ADDRESS=127.0.0.1:$((p + 1)) \
  RESTATE_BIND_ADDRESS=127.0.0.1:$((p + 2)) RESTATE_DISABLE_TELEMETRY=true
exec bin/restate-server --no-logo
