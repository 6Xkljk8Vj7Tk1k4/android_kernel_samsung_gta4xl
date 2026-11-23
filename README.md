# Linux 4.19 for the Samsung Galaxy Tab S6 Lite (`gta4xl`)

Linux 4.19.325 for the Galaxy Tab S6 Lite LTE (SM-P615, `gta4xl`, Exynos 9611).
Samsung never released a kernel newer than 4.14 for this SoC, and the 4.14
stable series ended in March 2026. This branch adds the tablet to Samsung's
4.19 tree for the Exynos 850, as LineageOS maintains it for Android 17
([android_kernel_samsung_exynos850](https://github.com/LineageOS/android_kernel_samsung_exynos850),
`lineage-24.0`, at `b8c71b7aecec`), and merges the 4.19 stable backports that
the Civil Infrastructure Platform has kept publishing since kernel.org ended
the series (`linux-4.19.y-st`, release `st21`: `uname -r` says
`4.19.325-st21-g<commit>`).

It is the kernel of an unofficial LineageOS 24.0 build for this tablet; the ROM
side (build environment, port patches) lives in
[lineageos-24.0-gta4xl-builder](https://github.com/6Xkljk8Vj7Tk1k4/lineageos-24.0-gta4xl-builder).
It replaces the 4.14 kernel of branch `gta4xl-24.0`, which is archived and no
longer maintained.

Only the `exynos9611-gta4xl_defconfig` configuration is supported. The Exynos
850 boards of the donor tree cannot be built from this branch any more (their
battery and sensor hub drivers made room for the ones this tablet needs), and
the Wi-Fi-only model (`gta4xlwifi`) is not ported.

## Status

Tested on a P615 with LineageOS 24.0 (Android 17), the ROM unchanged from the
one built for the 4.14 kernel.

| Area | State |
|---|---|
| Boot, UFS with inline encryption, SD card | works |
| Display, touch, S Pen, keys, cover sensors, grip sensors | works |
| GPU (Mali, `bv_r38p1`) | works |
| Wi-Fi, Bluetooth | works |
| Audio: speakers, microphone | works (wired headset and Bluetooth audio not listened to yet) |
| Cameras: front and rear, photo and 1080p30 video | works |
| Video: hardware decode; encoders AVC, HEVC, VP9, VP8, MPEG-4, H.263 | works |
| Sensors: rotation, light and the rest of the sensor hub | works |
| Modem | registers on LTE and carries mobile data with a SIM; calls and SMS not tested |
| GNSS | the receiver starts and reports satellites; a position fix not tested |
| Charging | works from USB, also with the tablet off; AFC and USB PD chargers not tested |
| Suspend and resume, wake-up by power key, alarm and Wi-Fi | works |
| CPU idle: cluster power-down, SICD | works |
| Thermal throttling of CPU, GPU and ISP, CPU hotplug | works, same steps as 4.14 |
| Restart, restart to recovery, power-off, restart after a panic | works |
| USB: adb, MTP, PTP, RNDIS tethering, MIDI | works |
| Wi-Fi hotspot | starts on `wlan1` and Android sets up forwarding (the chip is either a client or an access point, so the client is off meanwhile); no device connected through it yet |

Known gaps:

- No NCM gadget function (`f_ncm.c` does not compile on this tree); tethering uses RNDIS.
- No ramoops device and no `/dev/pmsg0`: the RAM log described below owns the
  pstore zones. Android therefore reports `reboot` instead of `kernel_panic`
  as the boot reason after a panic.
- binderfs has no `features` directory (extended errors, one-way spam detection).
- The voice-trigger DSP is not bound; no FM radio.
- `CONFIG_JUMP_LABEL` has to stay off: static keys do not flip on this tree, and
  netd's BPF start-up test then aborts.
- Mali CVE-2024-0671 and CVE-2025-0427 are open: the fixes exist only in driver
  releases newer than any public source drop.
- Three warnings at boot that the 4.14 kernel prints as well (a grip sensor
  sysfs file, two GPIO lookups of the RT5665).
- Seen once, cause unknown: after an unlock one `tz_worker_thread` entered the
  secure world in a loop (the secure OS kept its request mask at one core),
  which keeps that core busy until it is taken offline and online again or
  the tablet restarts. The TEE driver is the 4.14 kernel's.

## What is in the branch

1. **The port**: one commit per area (device trees, platform support, power,
   storage and PHYs, TEE, display, media, audio, modem and GNSS, input and
   sensors, RAM log, configuration).
2. **Fixes to the tree** that stand on their own, each a commit with its
   reasoning: performance (see below), CPU hotplug with cluster power-down,
   thermal limits for the v1 TMU driver, suspend and power modes of the 9610,
   reset reasons for this bootloader, dw_mmc and devfreq bindings,
   `kthread_unpark()`.
3. **Security fixes**: 74 commits, upstream fixes with their original authors
   and the fixes of the 4.14 branch for vendor code.
4. **CIP's `linux-4.19.y-st`**, merged: the 5036 backports made on top of
   4.19.325 between February 2025 and November 2026. The merge commit lists
   what was adapted and what was left out, and why.

### Where the code comes from

The tablet's bootloader, its secure OS and its closed Android components
(hwcomposer, camera HAL, RIL, sensor HAL, audio DSP firmware) fix many
interfaces. Where they do, the driver is the one of the tablet's 4.14 kernel,
adapted to 4.19; elsewhere it is this tree's.

| Area | Source | Why |
|---|---|---|
| Device trees | 4.14, unchanged | the bootloader and vendor code know them; DTB and DTBO are byte-identical to the 4.14 build |
| cal-if, ACPM with its plug-in firmware, PMU, TMU, reboot, suspend and power modes | 4.14 | formats of the firmware the bootloader loads; this SoC's PMU |
| UFS host glue, FMP, secure RPMB, USB DRD PHY, MIPI D-PHY | 4.14 | bindings and calibration of this SoC |
| S2MPU09 PMIC, SM5713 (MUIC, USB PD, charger, fuel gauge), battery | 4.14 | no driver in this tree |
| Display (dpu20), panel, bus traffic shaper | 4.14 | hwcomposer ioctls |
| Camera (fimc-is2), GDC | 4.14 | camera HAL and ISP library ABI |
| Audio (ABOX v1, RT5665, TAS2562) | 4.14 | this tree has ABOX v2/v3 only |
| Modem, AP-CP mailbox, GNSS interface | 4.14 | RIL and GNSS daemons |
| Touch, S Pen, hall and grip sensors, sensor hub | 4.14 | no driver in this tree; sensor HAL |
| TEE driver (tzdev) | 4.14 | the TEEGRIS version this bootloader starts |
| Video codec (MFC), scaler, JPEG, GPU, Wi-Fi/BT (SCSC), ION | this tree | same driver generation; fixes added |
| Everything else (core kernel, networking, filesystems, Android common) | this tree | |

## Performance

The kernel runs at EL1 under Samsung's hypervisor, which makes some things
unusually expensive on this board. Measured on the tablet, same ROM:

- Every write to `TTBR0_EL1`, `TTBR1_EL1` or `CONTEXTIDR_EL1` is trapped
  (about 140 ns on a big core, 180 ns on a little one). The software PAN
  emulation writes two of them on each kernel entry and exit.
  `CONFIG_ARM64_ASID_IN_TTBR0` keeps the ASID in `TTBR0_EL1`, so one write
  carries table and ASID.
- The workaround for Cortex-A73 erratum 858921 costs the vDSO clock on 4.14:
  every clock read of every app is a system call.
  `CONFIG_ARM64_ERRATUM_858921_VDSO` does the double read in the vDSO.
- The cores have no hardware access flag, so fault-around pages are mapped
  young.
- All UFS interrupts land on CPU0, which sleeps in cluster power-down between
  requests; the host asks for a CPU latency limit while it is active.
- The vendor tree started with an audit rule count of 1, so every system call
  collected names and paths.

| | 4.14 | this branch | with `gta4xl_fast.config` |
|---|---|---|---|
| System call (`getppid`), big core | 754 ns | 494 ns | 183 ns |
| Anonymous page fault, big core | 1938 ns | 1353 ns | 1050 ns |
| Cached file page through a mapping | 777 ns | 347 ns | 297 ns |
| Random 4k direct reads | 3178 /s | 4458 /s | 5034 /s |
| Clock read by an app, big / little core | 1446 / 2204 ns | 56 / 76 ns | 56 / 76 ns |
| App cold start, mean of five apps | | 7 % faster | 11 % faster |

The default configuration keeps the two mitigations the 4.14 kernel has,
software PAN and the Spectre-v2 firmware call on context switch (about 7 µs
per process switch on this firmware). `gta4xl_fast.config` turns both off:

```sh
make exynos9611-gta4xl_defconfig gta4xl_fast.config
```

UI animations are limited by the GPU and the compositor on both kernels, not
by the kernel.

## Security

Samsung's kernel source for this SoC has had next to no security fixes since
mid-2024. kernel.org ended 4.19 with 4.19.325 in December 2024; the Civil
Infrastructure Platform (CIP) continues it, and this branch merges CIP's
stable-style branch. That covers the generic kernel as far as CIP backports
it. It does not cover the vendor drivers, Android's additions, or fixes that
no 4.19 branch ever received. Those are picked one by one, from the oldest
stable branch that has them or ported by hand (noted in the commit message):

- **64 CVEs fixed by commits of this branch**: everything the 4.14 branch
  had, among them CVE-2024-4610 and CVE-2023-26083 (Mali, both exploited in
  the wild), CVE-2025-38352 (posix
  CPU timers, used against Android) and the remote ones in IPv4, IPv6, TCP and
  cfg80211 such as CVE-2026-98432, plus those found by checking this tree.
  The donor tree lacked the Mali, ION and Wi-Fi driver fixes although it has
  the same driver generation.
- **Bugs without a CVE** in vendor code that an app or an access point can
  reach: two in Samsung's ION allocator and six in the SCSC Wi-Fi driver's
  receive path, among them an out-of-bounds write that an access point could
  trigger with its association response before any keys are exchanged.
- **Faults that only 4.19 has** and an app can reach: CVE-2024-26759 (zram
  swap-in without the swap cache races between threads and loses writes),
  CVE-2024-26640 (TCP receive zero-copy mapped page cache pages) and
  CVE-2026-97958 (a netlink loop under RTNL).
- **Configuration** as on the 4.14 branch: SLAB freelist randomisation and
  hardening, `BUG_ON_DATA_CORRUPTION`, `PANIC_ON_OOPS`, restricted `dmesg`,
  SYN cookies, drivers and protocols the tablet does not have removed; in
  addition stack variables are zero-initialised and slab caches are not merged
  (an object freed too early cannot be handed out again as an object of
  another cache's kind while its page stays in use). Of the options Android
  asks of a 4.19 kernel (`android-base.config`), 17 are left out on purpose:
  Mobile IPv6, VTI tunnels, PPP with L2TP and PPTP, DCCP connection tracking
  and the connection tracking helpers Android never attaches. The two it does
  attach when tethering starts, FTP and PPTP, are built: without either one
  netd's rule batch fails and tethering does not come up. The builder
  repository's tablet test compares the configuration with Android's list and
  holds the reason for each option that is missing.

Every new kernel CVE is checked against the files this configuration actually
compiles (kernel CNA `vulns.git`, headers through kbuild's `.o.cmd` lists);
the tooling is in the builder repository. State of 2026-11-17 for the network
and browser-sandbox surfaces: 630 CVEs touch compiled files, 389 are fixed
(164 of them by the CIP merge), 241 are open, each with a recorded verdict:
74 need root or a capability, 57 cannot be reached in this configuration or
with Android's defaults, 62 concern code that is not built or not present, 40
are of low impact, 7 were judged not worth a risky port. One is reachable by
an app: CVE-2022-50445, a soft lockup through inbound ESP on the app's own
IPsec association; its fix is a rework of the receive path.

Not taken from CIP, and therefore still open here:

- **f2fs**: this tree's f2fs is much newer than 4.19's, and CIP's 22 f2fs
  fixes are written for the old code. `/data` is on f2fs, so each of them has
  to be taken from mainline and reviewed on its own. The merge commit lists
  them.
- **USB gadget**: the race between `gether_disconnect()` and `eth_stop()`
  (upstream `e1eabb072c75`). The vendor's version of that code dereferences
  the pointer the fix clears early; it needs its own port. The halt timeout
  fix for `dwc3_gadget_run_stop()` is not applied either: the vendor function
  handles that case differently.

## Building

With the LineageOS build nothing changes on the device side:
`TARGET_KERNEL_CONFIG := exynos9611-gta4xl_defconfig`, `LLVM=1 LLVM_IAS=1`, the
ROM's clang (tested with clang-r596125), image name `Image`, the same DTB and
DTBO lists. The device trees built from this branch are byte for byte those
of the 4.14 kernel.

Standalone, with a distribution's clang and lld (tested with 21.1):

```sh
make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
     exynos9611-gta4xl_defconfig
make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
     -j"$(nproc)" Image dtbs
```

The bootloader ignores the command line of the boot image, so kernel options
go into `CONFIG_CMDLINE`.

## Android side

Tested with a ROM built for the 4.14 kernel, without changes:

- `ro.bpf.kver_override=5.10.239` as a product property, as for the 4.14
  branch;
- the open HALs built against the 4.14 kernel headers
  (`hardware/samsung_slsi-linaro`): the video codec driver accepts the
  encoder controls under their 4.14 ids too;
- no sepolicy changes.

## Debugging without a UART

The tablet has no accessible serial port. `CONFIG_GTA4XL_RAMLOG`
(`arch/arm64/kernel/gta4xl_early_ramlog.c`) is what the port was debugged
with, and stays on:

- Every kernel log line also goes into the board's pstore console zone, from
  the first lines of `setup_arch()`. `/proc/gta4xl_last_kmsg` (root) holds the
  end of the start before this one, however it ended, and a line in the
  current log says how:
  `GTA4XL: the start before this one ended: ...`. A 4.14 kernel, for instance
  in a recovery image, shows the same zone as
  `/sys/fs/pstore/console-ramoops-0`.
- `/proc/reset_reason` tells what the bootloader saw: `RPON` restart, `KPON`
  panic.
- A panic in the first two minutes after start, or a start that finds its
  predecessor died in that phase, ends in the recovery, so a kernel that
  cannot come up does not loop. Later a panic restarts Android.
- Test boots: `make exynos9611-gta4xl_defconfig gta4xl_test.config` builds an
  image that resets to the recovery after 90 s and on any restart, panic or
  hang, and never suspends. With a recovery that has adb, a kernel can be
  tried from the boot partition and its log collected without touching the
  tablet. `gta4xl_cp=N` resets at checkpoint N of the boot.

## Updating

Once a month:

```sh
git remote add cip https://git.kernel.org/pub/scm/linux/kernel/git/cip/linux-cip.git
git fetch --no-tags cip linux-4.19.y-st
git merge cip/linux-4.19.y-st
```

Then the CVE check of the builder repository for what the merge did not bring.
Two things a merge of that branch needs every time: `fs/f2fs` stays as it is
here, and a change that merges without a conflict is not yet a change that
is right. CIP's fixes are written for plain 4.19, and where Samsung or
Android rewrote a function they can apply cleanly and still be wrong (the
first merge had one that would have crashed on USB tethering). Read every
hunk that lands in a function this tree has changed.

Other fixes go on top as single commits with the upstream reference
(`commit X upstream` or `cherry picked from commit X`). To follow the donor,
merge `LineageOS/android_kernel_samsung_exynos850` `lineage-24.0`.

## Credits

Samsung's open source releases for the SM-P615 and the Exynos 850 devices,
the LineageOS maintainers of `android_kernel_samsung_gta4xl` and
`android_kernel_samsung_exynos850`, the Civil Infrastructure Platform for
keeping 4.19 alive, and the kernel CNA for `vulns.git`.
