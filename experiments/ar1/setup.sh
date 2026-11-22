#!/usr/bin/env bash
# Installs the pinned AR-1 toolchain into $AR1_ENV (default /opt/ar1): PostgreSQL 18.7, Python 3.12, DBOS 3.6.0.
set -euo pipefail
ENV=${AR1_ENV:-/opt/ar1}
MM=${AR1_MM:-/tmp/ar1-mm}
mkdir -p "$MM"
curl -sSL -o "$MM/mm.tar.bz2" https://conda.anaconda.org/conda-forge/linux-64/micromamba-2.9.0-0.tar.bz2
tar -xjf "$MM/mm.tar.bz2" -C "$MM" bin/micromamba
MAMBA_ROOT_PREFIX="$MM/root" "$MM/bin/micromamba" create -y -q -p "$ENV" -c conda-forge postgresql=18.7 python=3.12
"$ENV/bin/python3" -m pip install -q -r "$(dirname "$0")/requirements.txt"
"$ENV/bin/postgres" --version
