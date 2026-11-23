#!/usr/bin/env bash
# Installs the locked AR-1 toolchain into $AR1_ENV (default /opt/ar1): PostgreSQL 18.7, Python 3.12, DBOS 3.6.0.
# Every conda package and wheel is checked against the sha256 in conda-linux-64.lock and requirements.lock.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ENV=${AR1_ENV:-/opt/ar1}
MM=${AR1_MM:-/tmp/ar1-mm}
mkdir -p "$MM"
curl -sSL -o "$MM/mm.tar.bz2" https://conda.anaconda.org/conda-forge/linux-64/micromamba-2.9.0-0.tar.bz2
echo "$(cat "$HERE/micromamba.sha256")  $MM/mm.tar.bz2" | sha256sum -c -
tar -xjf "$MM/mm.tar.bz2" -C "$MM" bin/micromamba
MAMBA_ROOT_PREFIX="$MM/root" "$MM/bin/micromamba" create -y -q -p "$ENV" --file "$HERE/conda-linux-64.lock"
"$ENV/bin/python3" -m pip install -q --require-hashes --no-deps --only-binary=:all: -r "$HERE/requirements.lock"
"$ENV/bin/postgres" --version
