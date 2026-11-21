#!/bin/sh
# Prints the TLC config for one L1Authority run (docs/AFR_001_L1_AUTHORITY.md section 6).
set -eu
cfg=$(dirname "$0")/L1Authority.cfg
case $1 in
  L1-F) sed 's/^INVARIANT .*/INVARIANT OneAuthority/' "$cfg" ;;
  L1-Fi) sed 's/OpInside = FALSE/OpInside = TRUE/' "$cfg" ;;
  L1-Fr) sed 's/R2p = FALSE/R2p = TRUE/' "$cfg" ;;
  L1-mA1) sed 's/FixA1 = TRUE/FixA1 = FALSE/' "$cfg" ;;
  L1-mVer) sed 's/FixVerCAS = TRUE/FixVerCAS = FALSE/' "$cfg" ;;
  L1-mDec) sed 's/FixDecide = TRUE/FixDecide = FALSE/' "$cfg" ;;
  L1-P) sed 's/^INVARIANT .*/INVARIANT NoL1Window/' "$cfg" ;;
  *) echo "unknown run $1" >&2; exit 1 ;;
esac
