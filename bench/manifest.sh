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
echo "### memory and huge pages (/proc/meminfo)"
grep -E '^(MemTotal|MemAvailable|SwapTotal|HugePages_|Hugepagesize|Hugetlb)' /proc/meminfo 2>/dev/null
echo
for n in /sys/devices/system/node/node*; do
  [ -r "$n/meminfo" ] && grep -E 'MemTotal|HugePages_Total' "$n/meminfo"
done
echo
run lsblk -o NAME,MODEL,SIZE,ROTA,TYPE,MOUNTPOINT,FSTYPE
run lspci -nn
echo "### cpufreq governor (per CPU, unique)"
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c || echo "(no cpufreq)"
echo
echo "### cpufreq driver, limits, turbo"
for f in scaling_driver scaling_min_freq scaling_max_freq cpuinfo_max_freq energy_performance_preference; do
  printf '%s: ' "$f"; cat /sys/devices/system/cpu/cpu0/cpufreq/$f 2>/dev/null || echo "(n/a)"
done
printf 'intel_pstate no_turbo: '; cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo "(n/a)"
printf 'intel_pstate status: '; cat /sys/devices/system/cpu/intel_pstate/status 2>/dev/null || echo "(n/a)"
printf 'boost: '; cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo "(n/a)"
echo "### cpuidle states (cpu0)"
for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do
  [ -r "$s/name" ] && echo "$(cat $s/name) latency_us=$(cat $s/latency) disabled=$(cat $s/disable)"
done
printf 'cpuidle driver: '; cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null || echo "(n/a)"
echo
echo "### transparent hugepages"
cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null
cat /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null
echo
echo "### kernel command line and scheduling knobs"
cat /proc/cmdline
for k in kernel/numa_balancing vm/swappiness vm/dirty_ratio vm/dirty_background_ratio kernel/sched_autogroup_enabled; do
  printf '%s: ' "$k"; cat /proc/sys/$k 2>/dev/null || echo "(n/a)"
done
printf 'isolated cpus: '; cat /sys/devices/system/cpu/isolated 2>/dev/null || echo "(n/a)"
printf 'nohz_full: '; cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo "(n/a)"
echo
echo "### BIOS and memory DIMMs (dmidecode; needs root)"
if command -v dmidecode >/dev/null 2>&1 && [ "$(id -u)" = 0 ]; then
  dmidecode -t bios -t system 2>/dev/null | grep -E 'Vendor|Version|Release Date|Manufacturer|Product Name' | grep -v 'Serial'
  dmidecode -t memory 2>/dev/null | grep -E '^\s+(Size|Speed|Configured Memory Speed|Type|Locator|Rank):' | grep -v 'No Module' | sort | uniq -c
else
  echo "(run as root with dmidecode for BIOS and DIMM details)"
fi
echo
echo "### data dir: $data"
run df -hT "$data"
run findmnt -T "$data" -o TARGET,SOURCE,FSTYPE,OPTIONS
echo "### block devices: scheduler, write cache, queue (per disk)"
for q in /sys/block/*/queue; do
  d=$(basename "$(dirname "$q")")
  case "$d" in loop*|ram*|zram*) continue;; esac
  echo "$d: rotational=$(cat $q/rotational 2>/dev/null) scheduler=$(cat $q/scheduler 2>/dev/null) write_cache=$(cat $q/write_cache 2>/dev/null) nr_requests=$(cat $q/nr_requests 2>/dev/null) fua=$(cat $q/fua 2>/dev/null)"
done
echo
run nvme list
for nic in "$@"; do
  run ethtool "$nic"
  run ethtool -i "$nic"
  run ethtool -l "$nic"
  run ethtool -x "$nic"
  run ethtool -k "$nic"
  run ethtool -c "$nic"
  run ethtool -g "$nic"
  echo "### $nic: NUMA node $(cat /sys/class/net/$nic/device/numa_node 2>/dev/null), MTU $(cat /sys/class/net/$nic/mtu 2>/dev/null)"
  echo "### $nic: interrupts (count of IRQ lines, and their CPU affinity)"
  grep -c "$nic" /proc/interrupts 2>/dev/null
  for irq in $(grep "$nic" /proc/interrupts 2>/dev/null | cut -d: -f1 | tr -d ' '); do
    printf '%s:%s ' "$irq" "$(cat /proc/irq/$irq/smp_affinity_list 2>/dev/null)"
  done
  echo
  echo
done
printf 'irqbalance: '; (systemctl is-active irqbalance 2>/dev/null || echo unknown)
echo
run ip -br link
echo "### virtualization"
run systemd-detect-virt
echo "### build environment"
run c++ --version
run cmake --version
run ninja --version
run python3 --version
run ldd --version
echo "### experiment network topology"
echo "(fill in by hand: which hosts share which switch or link, link speeds, and the role of each host)"
