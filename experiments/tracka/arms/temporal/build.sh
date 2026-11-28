#!/bin/sh
set -eu
cd "$(dirname "$0")"
V=1.32.2
T=temporal_${V}_linux_amd64.tar.gz
SHA=6260f110b6853d2d7f8daa31793d1fac44096077e3b058dc3482de6ac50bc543
if [ ! -x bin/temporal-server ]; then
  mkdir -p bin
  curl -fsSL -o bin/$T https://github.com/temporalio/temporal/releases/download/v$V/$T
  echo "$SHA  bin/$T" | sha256sum -c -
  tar xzf bin/$T -C bin temporal-server temporal-sql-tool
  rm bin/$T
fi
sha256sum bin/temporal-server bin/temporal-sql-tool
[ -x .venv/bin/python ] || /opt/ar1/bin/python3 -m venv .venv
.venv/bin/pip install -q --require-hashes -r requirements.lock
