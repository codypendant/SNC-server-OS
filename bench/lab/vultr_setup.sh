#!/usr/bin/env bash
# Prepare a fresh Ubuntu 24.04 VPS for the three-host lab (bench/lab/vultr.toml).
#
#   curl -fsSL https://raw.githubusercontent.com/codypendant/SNC-server-OS/main/bench/lab/vultr_setup.sh | bash -s -- A_IP B_IP C_IP
#
# Run it as root on a and b first, then on c. Every host gets the packages and
# opens tcp 21000-21600 to the other two (ufw drops everything but SSH by
# default, which silently kills node links and witnesses). On c it also clones
# and builds the repository, installs c's SSH key on a and b (asks each root
# password once), writes the new addresses into bench/lab/vultr.toml and
# deploys the binaries, so the lab is ready for bench/lab/lab.py.
set -euo pipefail
if [ $# -ne 3 ]; then
  echo "usage: $0 A_IP B_IP C_IP" >&2
  exit 2
fi
A=$1 B=$2 C=$3
REPO=${SNC_REPO:-https://github.com/codypendant/SNC-server-OS.git}
DIR=${SNC_DIR:-$HOME/snc-server-os}

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -yq build-essential cmake ninja-build git liburing-dev libmimalloc-dev libxxhash-dev \
  iperf3 fio iproute2 iptables python3

if command -v ufw >/dev/null; then
  for ip in "$A" "$B" "$C"; do
    ufw allow from "$ip" proto tcp to any port 21000:21600 >/dev/null
  done
  echo "ufw: tcp 21000-21600 open to $A $B $C"
fi

me=""
for ip in $(hostname -I); do
  case "$ip" in
    "$A") me=a ;;
    "$B") me=b ;;
    "$C") me=c ;;
  esac
done
if [ -z "$me" ]; then
  echo "this host's addresses ($(hostname -I)) are none of $A $B $C" >&2
  exit 1
fi
if [ "$me" != c ]; then
  echo "host $me ready. Now run this script on c ($C)."
  exit 0
fi

if [ -d "$DIR/.git" ]; then
  git -C "$DIR" pull --ff-only
else
  git clone "$REPO" "$DIR"
fi
cd "$DIR"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

[ -f "$HOME/.ssh/id_ed25519" ] || ssh-keygen -q -t ed25519 -N "" -f "$HOME/.ssh/id_ed25519"
for ip in "$A" "$B"; do
  if ! ssh -n -o BatchMode=yes -o StrictHostKeyChecking=accept-new "root@$ip" true 2>/dev/null; then
    echo "Enter root's password on $ip:"
    ssh-copy-id -o StrictHostKeyChecking=accept-new "root@$ip" </dev/tty
  fi
  ssh -n -o BatchMode=yes "root@$ip" true
done

python3 - "$A" "$B" "$C" <<'PY'
import re, sys
a, b, c = sys.argv[1:]
p = "bench/lab/vultr.toml"
s = open(p).read()
for name, ip in (("a", a), ("b", b), ("c", c)):
    s = re.sub(r'(\[hosts\.%s\]\naddr = ")[^"]*(")' % name, r"\g<1>%s\2" % ip, s)
    if name != "c":
        s = re.sub(r'(\[hosts\.%s\]\naddr = "[^"]*"\nssh = "root@)[^"]*(")' % name, r"\g<1>%s\2" % ip, s)
open(p, "w").write(s)
PY
grep -A3 '^\[hosts' bench/lab/vultr.toml
python3 bench/lab/lab.py bench/lab/vultr.toml deploy
echo "lab ready in $DIR: python3 bench/lab/lab.py bench/lab/vultr.toml p0"
