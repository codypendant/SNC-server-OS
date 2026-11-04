#!/bin/sh
# Machine manifest for benchmark evidence (docs/EXPERIMENTAL_VALIDATION.md
# section 8). Run on every host before its first benchmark, and again after
# any hardware, kernel, BIOS or storage change. Commit the output under
# bench/manifests/ next to the results that used the host.
#
#   bench/manifest.sh NAME [DATA_DIR] [NIC ...] > bench/manifests/NAME-YYYYMMDD.txt
#
# NAME: a short label (no hostnames or addresses needed). DATA_DIR: where the
# SNC data directories go (its filesystem and mount options are recorded).
# NIC: the test interfaces (ethtool). Missing tools are reported, not fatal.
# Nothing here prints secrets; it does print hardware serial-free inventory.
# Review the file before committing (it may include public IP addresses in
# `ip -br addr`: remove them if the host is not disposable).
name=${1:?usage: manifest.sh NAME [DATA_DIR] [NIC ...]}
shift
data=${1:-.}
[ $# -gt 0 ] && shift

run() {
  echo "### $*"
  if command -v "$1" >/dev/null 2>&1; then "$@" 2>&1; else echo "($1 not installed)"; fi
  echo
}

echo "# manifest $name"
echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "snc_commit: $(git -C "$(dirname "$0")/.." rev-parse HEAD 2>/dev/null || echo unknown)"
echo
run uname -a
[ -r /etc/os-release ] && { echo "### /etc/os-release"; cat /etc/os-release; echo; }
run lscpu
run lscpu -e
run numactl --hardware
run free -h
run lsblk -o NAME,MODEL,SIZE,ROTA,TYPE,MOUNTPOINT,FSTYPE
run lspci -nn
echo "### cpufreq governor (per CPU, unique)"
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c || echo "(no cpufreq)"
echo
echo "### transparent hugepages"
cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null
echo
echo "### data dir: $data"
run df -hT "$data"
run findmnt -T "$data" -o TARGET,SOURCE,FSTYPE,OPTIONS
for nic in "$@"; do
  run ethtool "$nic"
  run ethtool -i "$nic"
done
run ip -br link
echo "### virtualization"
run systemd-detect-virt
