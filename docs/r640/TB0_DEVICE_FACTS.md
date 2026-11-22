# R640 card TB-0: data SSD facts and flush latency (read-only except one scratch file)

For Claude Code on the R640. Run exactly these steps, in order, and nothing else. Do not tune, install anything not
named here, or start any benchmark. If a step fails, stop and report the output.

**Why:** the Track B plan (`docs/DIRECTION_COMPARISON_PLAN.md` 4.1) lists the data SSD's power-loss protection and its
`fua=1` path as UNRESOLVED. This card records the device's identity, its cache settings, and how long a flush takes.
It does not settle power-loss protection on its own; the identity is checked against the vendor's documents later.

**Do not touch** the old VMFS HDD or any device other than the one under `/srv/snc-data`. The only write is a scratch
directory `/srv/snc-data/dc1-tb0`, deleted in step 5.

## 1. Update and check nothing is running
```bash
cd /home/claude/snc-server-os && git pull --ff-only && git log --oneline -1
pgrep -a 'snc_server|net_bench|fio' || echo "nothing running"
```
If anything is running, stop and report.

## 2. Find the data device by its mount, never by name
```bash
PART=$(findmnt -no SOURCE /srv/snc-data); DEV=/dev/$(lsblk -no PKNAME "$PART"); echo "$PART $DEV"
lsblk -o NAME,MODEL,SERIAL,SIZE,ROTA,MOUNTPOINT "$DEV"
```
The model must be the Samsung `ARFX3840S5xnFTRI`. If it is not, stop and report.

## 3. Identity and cache settings
`smartmontools`, `sg3-utils` and `sdparm` are read-only tools. Install them only if they are missing:
```bash
command -v smartctl sg_vpd sdparm fio || sudo apt-get install -y smartmontools sg3-utils sdparm fio
OUT=/home/claude/snc-server-os/bench/results/dc1-trackb/tb0-r640-device; mkdir -p "$OUT"
B=$(basename "$DEV")
{ date -u; uname -r; for f in write_cache fua rotational scheduler; do echo "$f: $(cat /sys/block/$B/queue/$f)"; done; } > "$OUT/sysfs.txt"
sudo smartctl -x "$DEV" > "$OUT/smartctl.txt" 2>&1
sudo sg_inq "$DEV" > "$OUT/sg_inq.txt" 2>&1
sudo sg_vpd --all "$DEV" > "$OUT/sg_vpd.txt" 2>&1
sudo sdparm --all --long "$DEV" > "$OUT/sdparm.txt" 2>&1
cat "$OUT/sysfs.txt"; grep -iE "vendor|product|revision|serial|power|cache|WCE" "$OUT/smartctl.txt" "$OUT/sdparm.txt" | head -40
```

## 4. Flush and FUA latency (about 2 minutes)
Each job writes 4 KiB blocks with queue depth 1 to a 1 GiB file in the scratch directory.
```bash
mkdir -p /srv/snc-data/dc1-tb0
for mode in "--fdatasync=1" "--sync=dsync" ""; do
  name=$(echo "x$mode" | tr -dc 'a-z')
  fio --name="$name" --filename=/srv/snc-data/dc1-tb0/f --size=1G --rw=randwrite --bs=4k --direct=1 \
      --ioengine=psync --iodepth=1 --runtime=30 --time_based $mode --output-format=json > "$OUT/fio-$name.json"
done
python3 - "$OUT" <<'PY'
import json, sys, glob
for f in sorted(glob.glob(sys.argv[1] + "/fio-*.json")):
    j = json.load(open(f))["jobs"][0]
    w, s = j["write"], j.get("sync", {})
    print(f.split("/")[-1], "iops", round(w["iops"]), "write p50/p99 us",
          w["clat_ns"]["percentile"]["50.000000"] // 1000, w["clat_ns"]["percentile"]["99.000000"] // 1000,
          "sync p99 us", s.get("lat_ns", {}).get("percentile", {}).get("99.000000", 0) // 1000)
PY
```
The three jobs are: `xfdatasync` (a write, then a flush), `xsyncdsync` (`O_DSYNC`, a FUA write on this device), and
`x` (no sync, the reference).

## 5. Clean up and report
```bash
rm -rf /srv/snc-data/dc1-tb0 && ls /srv/snc-data
cd /home/claude/snc-server-os && git checkout -b r640-dc1-tb0 && git add bench/results/dc1-trackb/tb0-r640-device \
  && git commit -m "dc1 TB-0: R640 data SSD identity, cache settings and flush latency" && git push -u origin r640-dc1-tb0
```
Report to cody: the device line from step 2, `sysfs.txt`, the grep output from step 3, the fio summary from step 4,
and whether the push worked. Do not interpret the numbers.
