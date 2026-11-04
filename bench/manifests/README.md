# Machine manifests

Every host that produces benchmark evidence has a manifest here, recorded
before its first run and again after any hardware, kernel, BIOS or
storage change ([EXPERIMENTAL_VALIDATION.md](../../docs/EXPERIMENTAL_VALIDATION.md)
section 8). Do not guess CPU topology: read it from the manifest.

    bench/manifest.sh NAME DATA_DIR NIC... > bench/manifests/NAME-YYYYMMDD.txt

Review the file before committing it: remove addresses of hosts that are
not disposable. Results under `bench/results/<campaign>/` name the
manifests of every host they used, plus the SNC commit SHA and exact
command lines.

| Host | Role | Manifest |
| --- | --- | --- |
| This project's cloud VM (4 vCPU, 15 GiB, one NUMA node) | Development, loopback regression checks. Not experiment evidence: multi-process runs are CPU-oversubscribed | [dev-vm-4vcpu-20261104.txt](dev-vm-4vcpu-20261104.txt) |
| Vultr VPSs, 2026-11-04 (three, 2 vCPU) | Lab run P0-P6. Destroyed; no script manifest. What was recorded: [REAL_MACHINES.md](../../docs/REAL_MACHINES.md) | none |
| Main local server: Dell PowerEdge R640 (dual-socket Xeon Gold, about 384 GB; not the R740xd) | Bare-metal local SNC benchmarks | **to capture before first use** |
| R740xd | TrueNAS Scale; not used for bare-metal SNC tests | n/a |
| Remote physical host: the partner's Dell PowerEdge R640 (Linux) | WAN and cross-site class (kept separate from LAN results) | **to capture before first use**, with RTT, jitter and loss to the main host |
| VPS nodes | Same-region and WAN fabric, fault injection | at creation, with provider, instance type, region, and RTT, jitter and loss per pair |
