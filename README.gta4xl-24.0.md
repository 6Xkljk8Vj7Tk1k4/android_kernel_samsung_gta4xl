# gta4xl kernel for LineageOS 24.0 (branch `gta4xl-24.0`)

Linux 4.14.357 for the Samsung Galaxy Tab S6 Lite (`gta4xl` LTE, `gta4xlwifi`,
Exynos 9611) with the eBPF backports Android 17 expects at the "kernel 5.10"
level, the cgroup v2 freezer and security fixes that nobody else ships for
this 4.14 kernel any more. It is the kernel used by an unofficial LineageOS
24.0 build of this device; the ROM side lives in a separate builder
repository.

`gta4xl-24.0` is the only maintained branch. The former `gta4xl-24.0-bpf510`
(eBPF port only) and `gta4xl-24.0-mali-cves` (port + security fixes) were
folded into it; their history is fully contained here.

## Why

LineageOS 24.0 (Android 17) requires eBPF features of kernel 5.10 — ring
buffers, mmapable maps, `BPF_CGROUP_INET_SOCK_RELEASE`, `bpf_link` — and uses
the cgroup v2 freezer for cached apps. The `lineage-23.2` kernel only carries
the backports up to the 5.4 level, so Android 17 either boot-loops
(`bpfloader-failed`, `netd1shot-fail`) or needs platform patches, and apps are
never frozen. Official LineageOS 24.0 devices on 4.14/4.19 kernels (Motorola
Exynos 9610, Qualcomm sm8150/sm7125, Samsung exynos850/universal9830) ship the
same backport series; this branch brings it to gta4xl.

## What is in the branch

Base: `LineageOS/android_kernel_samsung_gta4xl` `lineage-23.2` at
`8d1f7a68b0d0` (4.14.356).

1. **Merge of `PhoenixKernel/linux` `android-4.14-phoenix` at `f515e763d18e`**
   ("Merge branch 'bpf-5.10'", 2026-03-14, 1,360 commits). Phoenix is this
   kernel's upstream (it was last merged into gta4xl in January 2026); it adds
   openela 4.14.357 and the eBPF-5.10 series (`kernel/bpf/ringbuf.c`,
   `bpf_iter.c`, `bpf_lsm.c`, `BPF_F_MMAPABLE`, `BPF_LINK_*`, sleepable
   programs, `CAP_BPF`/`CAP_PERFMON`), the cgroup v2 freezer
   (`kernel/cgroup/freezer.c`; the v1 one moved to `legacy_freezer.c`) and
   bumps the uname spoof for `bpfloader`/`netbpfload`/`netd` to `5.10.239`.
   The 15 conflicts were all in Samsung vendor code (KNOX NPA socket options,
   FIVE integrity hooks, SEC_BOOTSTAT, FREECESS, forced `AUDIT_ON`, Samsung's
   TSQ tweak, dwc3 runtime PM); the merge commit message lists the resolution
   per file.
2. **58 commits cherry-picked from `LineageOS/android_kernel_motorola_exynos9610`
   `lineage-24.0`** (the official 24.0 Exynos 4.14 device): 18 cgroup/freezer
   fixes (frozen state in `ptrace_stop`, inheritance, spurious unfreeze,
   `cgroup.threads` delegation), `clone3` with the arm64 `copy_thread_tls` fix,
   `CAP_CHECKPOINT_RESTORE` (`cap_last_cap` = 40), tipc/nfc fixes, rtmutex
   fixes.
3. **Two build fixes in Samsung's Mali `bv_r38p1` driver**: its pre-5.4
   compatibility shims redefined `fallthrough` and `__poll_t`, both now
   provided by the backported core headers.
4. **24 security fixes** (see below).

Nothing under `arch/arm64/boot/dts` changed, so the DTB/DTBO are identical to
`lineage-23.2`. `CONFIG_MODULES=y` but no module
is built as `=m`.

## Security fixes

Samsung's Exynos 9611 code was last updated with the 2024-05 security patch
level, and the openela 4.14 stable series stopped at 4.14.357 (2026-03). The
threat model for picking fixes is remote code execution over Wi-Fi or from a
web page, i.e. what a host on the same network or a compromised browser
renderer can reach. Fixes are taken from the oldest stable branch that has
them (or hand-ported where 4.14 differs, noted in each commit message):

- **Mali GPU (`bv_r38p1`)**: CVE-2024-4610 (CISA KEV; r41p0 no-user-free
  refcount and shrinkable checks, backported from r42p0), CVE-2023-26083
  (CISA KEV; the r38 drops compiled the "development" default that lets every
  app open the kernel-address-leaking timeline stream, now denied by default
  as in r43p0), and the two driver fixes from Samsung's P615XXSAFYD1 source
  drop (2 MB ATE only for full blocks, alias teardown offsets).
  CVE-2022-38181 and CVE-2023-4211 were already fixed by Samsung.
- **Wi-Fi (cfg80211)**: CVE-2026-98340 (hidden BSS grouping, reachable
  through scan results), CVE-2026-93805 and CVE-2026-93806 (MLME frame length
  checks; defence in depth, the SCSC full-MAC driver does not use those paths).
- **IPv4/IPv6/TCP receive path**: CVE-2026-74598 (RA Route Information
  length), CVE-2026-72323 (IGMP query UAF), CVE-2026-53275 (MLD query UAF),
  CVE-2026-53006 (`icmpv6_rcv`), CVE-2026-74398 (DAD failure race),
  CVE-2026-98255 (old ACKs in the TCP fast path), CVE-2024-56644 (expired PMTU
  exception dst leak), CVE-2025-68241 (stale fnhe rebinding), CVE-2026-97472
  (temporary addresses after prefix deprecation).
- **Browser-sandbox reachable**: binder UAFs CVE-2026-64469 and
  CVE-2026-64468, CVE-2025-38352 (CISA KEV, exploited on Android; posix CPU
  timer vs. reaped task), CVE-2026-64370, CVE-2026-98507 (anon_vma publish
  barrier), CVE-2025-40040 (KSM), CVE-2026-64109 (AF_UNIX `tail->len` UAF;
  own fix, because 4.14 still appends to the tail skb in
  `unix_stream_sendpage()`, so tail and length are read under the receive
  queue lock instead of dropping the length check as 6.6+ does).
- **USB (CISA KEV, forensic-tool exploit chain)**: CVE-2024-50302 (HID report
  buffer), CVE-2024-53197 (USB audio).

Every Linux kernel entry in the CISA KEV catalogue was checked: each is fixed
here, not built in this configuration, or was introduced after 4.14. Still
open in Mali: CVE-2024-0671 and CVE-2025-0427 (fixed only in DDKs newer than
any public source drop).

New kernel CVEs are reviewed monthly against the kernel CNA's `vulns.git`,
limited to files this configuration actually compiles (headers via kbuild's
`.o.cmd` dependency lists). A newer kernel is not an option: the Exynos 9611
vendor drivers (Mali, SCSC Wi-Fi/BT, modem, camera, display, audio, TEE) exist
only for 4.14 and talk to closed userspace blobs over private ioctl ABIs.

## Building

Same as the LineageOS build (`vendor/lineage/build/tasks/kernel.mk`):
`exynos9611-gta4xl_defconfig`, `LLVM=1 LLVM_IAS=1`, clang-r596125, no GCC.
Standalone, from an Android tree `$T` with its prebuilts:

```sh
make -j$(nproc) O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang LD=ld.lld \
  CLANG_TRIPLE=aarch64-linux-gnu- \
  HOSTCFLAGS="--sysroot=$T/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/sysroot -I$T/prebuilts/kernel-build-tools/linux-x86/include" \
  HOSTLDFLAGS="--sysroot=$T/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/sysroot -Wl,-rpath,$T/prebuilts/kernel-build-tools/linux-x86/lib64 -L $T/prebuilts/kernel-build-tools/linux-x86/lib64 -fuse-ld=lld --rtlib=compiler-rt" \
  LZ4=$T/prebuilts/kernel-build-tools/linux-x86/bin/lz4 PAHOLE=$T/prebuilts/kernel-build-tools/linux-x86/bin/pahole \
  DTC_EXT=$T/out/host/linux-x86/bin/dtc exynos9611-gta4xl_defconfig Image dtbs
```

The resulting `.config` differs from `lineage-23.2`'s only in new symbols
with safe defaults (`TASKS_TRACE_RCU=y`, `BPF_LSM`/`BPF_PRELOAD` unset).

**Do not enable `CONFIG_DEBUG_INFO_BTF`.** The backported
`scripts/link-vmlinux.sh` calls `resolve_btfids`, which this tree does not
have, so the link fails; and even with that fixed, pahole ≥ 1.24 emits BTF
kinds (ENUM64, enum flags) the backported `kernel/bpf/btf.c` rejects, after
which the verifier refuses every BPF program and the device boot-loops.
Android's BPF loaders do not need kernel BTF. Motorola and sm8150 ship with it
off as well.

## Android side

The device tree should report the backport level to the platform, as the
other 24.0 devices do:

```
# product.prop (TARGET_PRODUCT_PROP) — must be product, vendor_init cannot set it
ro.bpf.kver_override=5.10.239
```

The kernel's own uname spoof covers only root `bpfloader`/`netbpfload`/`netd`;
the property covers every BPF user (system_server, lmkd, …). With both, the
stock Android 17 Connectivity/netd/bpfloader run unmodified. No sepolicy or
cgroup profile changes are needed: `cgroups.json`/`task_profiles.json` already
describe the cgroup2 freezer and `FreezerState` → `cgroup.freeze`.

## Tested

On a P615 (LTE), last with `c1c07c3574ff` (2026-10-28, OTA update of a
running system): boots in under a minute; no kernel WARN/BUG in dmesg, no Mali
errors; `cap_last_cap` 40; `use_freezer=true`
(cached apps frozen); ring buffers pinned under `/sys/fs/bpf`; netd's native
5.10 stats programs, `BPF_CGROUP_INET_SOCK_RELEASE` and the platform ring
buffer test load; Wi-Fi (incl. WPA3), Bluetooth, camera, audio, sensors,
S Pen, GPS and FUSE passthrough working; no new SELinux denials compared to
the `lineage-23.2` kernel. Known and accepted on any LineageOS 4.14 kernel:
binder has no frozen-state change notifications (SensorService logs
`addFrozenStateChangeCallback ... error=-38`) and lmkd's two `KVER_6_1`-gated
memevents programs are skipped.

## Updating

New fixes go on top of `gta4xl-24.0` as single commits with the upstream
reference (`commit X upstream` / `cherry picked from commit X`), no parallel
branches.

Merge `PhoenixKernel/linux` `android-4.14-phoenix` again for newer openela
point releases (the tip after `f515e763d18e` adds AF_XDP and xfrm GSO
backports and two rtmutex fixes already cherry-picked here from Motorola), and
keep picking `android_kernel_motorola_exynos9610` `lineage-24.0` for
cgroup/freezer fixes.
