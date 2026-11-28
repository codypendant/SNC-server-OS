#!/bin/sh
cd "$(dirname "$0")"
exec "${CARGO_TARGET_DIR:-target}/release/tracka-restate" "$@"
