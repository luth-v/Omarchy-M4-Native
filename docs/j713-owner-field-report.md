# J713 owner field report: alpha.1 on a macOS 26.6.2 Air

Field report: October 13–14, 2026. One owner's 13-inch M4 Air (`Mac16,12`,
J713/T8132), installed from the v0.1.0-alpha.1 kit.

This records one installation from the first read-only checks to the current
state: the pre-install fixes, a silent boot hang in m1n1, its root cause and
binary fix, the GPU power-gate fix, a deep hardware test and the post-install
configuration. It is an owner's field report, not maintainer qualification.
Each claim is marked **observed**, **inferred** or **unverified** where the
difference matters. Times are the owner's local time.

## TL;DR

- **Install:** the kit installed cleanly once its AudioDSP pin accepted the
  macOS 26.6.2 (25G83) build, which renders byte-identical speaker filters.
- **Boot hang (observed, fixed):** the first boot stopped on the m1n1 logo
  with no text. Stage 1 loaded `boot.bin` (712 NVMe reads, loader returned
  0), then blocked forever writing console output to the dockchannel UART,
  whose TX FIFO nothing drains on this Air. The console writes UART →
  dockchannel → framebuffer, so nothing reached the screen.
- **Boot fix:** `dockchannel_uart_init()` is patched to return -1 (8 bytes),
  at offsets `0xf27c`/`0xf280` in stage 1 and `0xe80c`/`0xe810` in the
  stage-2 `loader.bin` inside `boot.bin`. The Air now boots unassisted. A
  proper fix belongs in upstream m1n1 and has not been reported yet.
- **GPU (observed, fixed):** the shipped `asahi_m4.ko` requires
  `apple,m4-max-power-mw == 24800`; this chip derives 24,676 mW, so the probe
  returned -22 silently. A one-byte patch (`b.ne` → `b.hi` at
  `.text+0x224c0`) accepts values up to 24,800. The firmware accepted the
  initialization graph; the desktop runs on `Apple M4 (G16G B1)`, GL 3.3 /
  GLES 3.0.
- **GPU crash (observed, cause unknown):** about 20 s into a sustained
  full-screen GLES load, the GPU firmware crashed with an MTR alarm. The
  session was lost, and graphics stayed dead until a reboot.
- **Works:** keyboard, touchpad, Wi-Fi on the previously untested USI
  module, Bluetooth, battery and charging, H.264/VP9 hardware decode, storage
  and CPU/memory stress. **Missing:** CPU DVFS (P-cores near 1.8 GHz, E-cores
  near 0.9 GHz), brightness, microphones, headphone jack, camera, external
  display and Vulkan. Suspend is not qualified.
- **Post-install fixes:** pinned the kernel package and `aquamarine`, set
  Brave to Wayland, hid Suspend, enabled TRIM, set the Wi-Fi regulatory
  domain, created the missing lock-screen PAM file and bound the lid switch
  under its actual name.

## Machine and baseline

| Item | This Air |
| --- | --- |
| Model | `Mac16,12`, J713AP, board `0x2c`, chip `0x8132`, T8132 stepping B1 |
| Memory / storage | 16 GB; APPLE SSD AP0512Z (512 GB class), ANS firmware 2973.120 |
| GPU | 10 cores (`apple,m4-core-mask` `0x3ff`), GPU ID `0x0a021100` rev `0x11` |
| Panel | 2560×1664 Retina |
| Wi-Fi/Bluetooth | BCM4388 rev C2, module `garden` WLMT 4.9, vendor **USI** |
| macOS | 26.6.2 (25G83), FileVault off, SIP on for macOS |
| System firmware (iBoot1 / SFR) | `mBoot-18000.161.10`, SFR `RestoreLongVersion` `25.7.83.0.0,0` |
| Stub OS firmware (iBoot2) | 26.5.1 (25F80), `mBoot-18000.120.36`; Linux sees this as `apple,firmware-build` |
| Fallback System Recovery | 26.3 |
| AudioDSP | `b8711ac5…`, 17,311,312 bytes; the kit pinned `5c52d15a…`, 17,360,016 bytes |
| Speaker ID | 3 (ADT `valid-speaker-ids = <0 3>`; the maintainer's value is unknown) |
| Kit | Omarchy M4 v0.1.0-alpha.1 J713 kit, downloaded October 13 with `download-j713.command` |
| Stub IPSW | `UniversalMac_26.5.1_25F80_Restore.ipsw`, SHA-256 `83f53dc7f9cce465ba88dd487bda74ce60c5582fcd45251eeeb9aea7f5a72f4d` |
| Stage 1 build | `air-c9ef56f-gpu-stage1-20261008` |
| Kernel | `7.1.9-m4-gpu-backport-20261008` #18 |

### Compared with the maintainer's unit

Before installing, the Air's IODeviceTree was captured from macOS, converted
back to an ADT and run through the project's own `tools/adt-audit.py`.
Everything the project depends on matched the published
[audit](t8132-adt-audit.md): all 27 `/arm-io` ranges, the 51 DT↔ADT node
rows, the 392-record PMGR table, the ANS secure BAR, the HPM/Type-C decode,
the MTP reset keys, the PMU shutdown scratchpad layout and the 4P+6E CPU
topology. The trackpad firmware IM4P was byte-identical to the pinned file.
These differed or could not be compared:

- **Wi-Fi/BT vendor:** USI here, AMKOR on the maintainer's unit. USI had not
  booted under this project before; it works.
- **System firmware:** `mBoot-18000.161.10` here; the project docs describe
  the maintainer's Air as running 26.5.1.
- **GPU top performance state:** 1470 MHz at **995 mV**. m1n1 derives
  24,676 mW from it. The shipped GPU module accepts exactly 24,800.
- **Speaker module ID:** unknown on the maintainer's side.

## Timeline

| Local time | Event |
| --- | --- |
| Oct 13 10:40 | Read the README, install guide, known limitations and [issue #57](https://github.com/Kb2uka/Omarchy-M4-Native/issues/57) (GPU -22 on another Mac16,12) |
| 10:51 | Read-only checks on macOS: model, 26.6.2 (25G83), `mBoot-18000.161.10`, AudioDSP mismatch |
| 11:06 | Owner speaker filters rendered from the 25G83 AudioDSP; all four outputs byte-identical to the pinned set |
| 11:53 | Kit and 26.5.1 IPSW downloaded; kit patched for 25G83; `Install.command --verify-only` passes (70/70) |
| 12:49–13:25 | Four read-only pre-install reviews (installer, boot chain, updates/audio, hardware identity) find the GPU gate (24,676 vs 24,800 mW); one-byte patch built and checked by disassembly |
| 13:35–15:33 | Three fix branches prepared, reviewed and corrected; fork PRs #1–#3 opened against the fork's `develop` |
| 18:08–18:22 | macOS automatic updates turned off; file-level backup of the owner's data copied off the machine |
| 18:26–18:37 | Installer run: speaker filters pass, macOS resized to 340 GB, Omarchy M4 installed, step 2 completed in 1TR |
| 18:42 | First boot: m1n1 logo, no text, unchanged after more than 10 minutes. Second attempt identical |
| 19:10–19:17 | From macOS: boot policy correct, ESP contents match the release, root filesystem never touched |
| 19:22–20:25 | ESP-only stage-2 experiments (no kernel, green logo, probe A): stage 2 never starts |
| 20:29–23:20 | Verbose-boot attempts (`boot-args=-v`, SIP off for the stub): no text, no USB proxy; boot-args found empty afterwards |
| 23:59 | First debug stage 1 installed with `kmutil` from the stub's paired 1TR |
| Oct 14 00:03 | 15 + 8 progress blocks: stage 1 stops before its jump to stage 2 |
| 00:18–00:56 | No-sleep variant boot-loops; "macOS needs to be reinstalled" screen; `kmutil` fails with "code pairing (17)" |
| 01:04–01:16 | `bless --setBoot` on the stub; paired 1TR confirmed with `bputil` |
| 01:21 | Read-counter build: 712 reads, loader returned 0, phase 3, then frozen |
| 01:28–01:34 | Dockchannel-disabled build: stage 2 runs and opens the USB proxy |
| 01:36 | Fixed `boot.bin` chainloaded over USB: Linux boots and first-boot setup runs |
| 01:39–01:52 | Text console (GPU -22, as predicted); USI Wi-Fi works; `apple,m4-max-power-mw` reads 24,676 |
| 01:55–02:02 | Fixed `boot.bin` written to the ESP from Linux; unassisted boot to Linux |
| 02:04 | Patched GPU module loaded by hand, then installed; reboot reaches the graphical login |
| 02:08–02:30 | Deep tests. GPU firmware crash at 02:28:34 under sustained load; a reboot recovers |
| 02:35–02:45 | Post-install fixes; lid lock confirmed by the owner |

## Pre-install work

### Read-only compatibility checks

The Mac was read over SSH from a Linux workstation with `sw_vers`,
`system_profiler`, `ioreg`, `diskutil`, `nvram -p` and `shasum`. Nothing was
written and nothing ran with `sudo`. Two blockers appeared:

1. **AudioDSP.** The installer renders the four J713 speaker FIR filters from
   the owner's AudioDSP and AID37 tuning strips before partitioning. It pins
   one AudioDSP hash, from 26.5.1/26.5.2. The 25G83 binary differs, so the kit
   refuses with `Owner filter generation stopped at run-transfer.py: ...
   AssertionError` and makes no changes.
2. **GPU power gate.** See [GPU power-gate analysis](#gpu-power-gate-analysis).

The installer review also found that macOS 26.7.1 and 27.0.1 were being
offered with automatic install on, and that J713's forced expert mode
produces a false "Overhead: 20.12 GB … Continue anyway? (y/N)" warning at the
resize step, whose default `N` exits the installer.

### AudioDSP 25G83 qualification (fork PR #3)

With the 25G83 hash swapped in, three places had to change: the manifest,
`run-transfer.py` and `run-memory.py`, which each hard-code the hash. Run with
the kit's bundled Python 3.13.9, helpers and NumPy under `env -i`, the
generator produced the four pinned outputs byte for byte: `cddb9a1c…`,
`c398560b…` and `c02bbed3…` twice. The installer handoff checks those four
hashes independently, so speaker-filter safety does not depend on the input
pin.

- **For this install:** a small script, `patch-kit-25g83-audiodsp.py`,
  swapped the pin in the kit and refreshed its `SHA256SUMS` lines. It
  refuses unless the Mac's AudioDSP is exactly the 25G83 build and the
  targets match `SHA256SUMS`. The originals were kept outside the kit.
  `Install.command --verify-only` then passed 70/70, and the real install
  passed the speaker-filter step.
- **Fork PR #3** (`fix/owner-fir-qualified-audiodsp`): manifest schema 2
  lists qualified `{sha256, size, macos}` entries per input while the outputs
  stay single-pinned. The release builder now assembles the owner-audio
  bundle from the repository apart from `bin/` and `dependencies/`; the old
  builder would have shipped stale `run-*.py` scripts from the helper
  tarball.

### GPU power-gate analysis

The shipped module `/opt/m4-gpu/asahi_m4.ko` (SHA-256 `80cf3669…`) is loaded
by `/usr/local/bin/m4-gpu-start` with `air_bootstrap_test=1`. It is not built
from the published kernel trees: their `drivers/gpu/drm/asahi/` has no
`air_bootstrap_test` and no Air path. Disassembly of the shipped binary found
the following.

- **The firmware check passes on a 26.5.1 stub.** `120.36` is accepted only
  with `air_bootstrap_test=1` and a root `compatible` containing both
  `apple,j713` and `apple,t8132`. A firmware mismatch would return -524, not
  -22.
- **The Air path has a silent board-constant gate** at
  `.text 0x22484–0x224c0`. Each check is an exact comparison:

  | Value | Required | This Air |
  | --- | --- | --- |
  | `apple,m4-core-mask` | `0x3ff` | `0x3ff` |
  | voltage groups | 10 | 10 (16 states fold into 10) |
  | top frequency | 1470 MHz | 1470 MHz |
  | `apple,m4-max-power-mw` | exactly 24800 | **24676** |

- **How m1n1 stage 2 derives the power.** It computes leakage plus dynamic
  current, clamps it at 24,800 mA and multiplies by the top voltage in
  single precision (`m4_power.h`, `kboot_gpu.c`). This Air has no
  `gpu-supports-higher-mA`, and its dynamic current at the top state is
  about 26,594 mA, above the clamp. The result is
  `(uint32)(0.995f × 24800.0f)` = 24,676, independent of the leakage fuse.
  Only a chip whose top state is exactly 1000 mV produces 24,800.
- **What happens on failure.** Any mismatch returns -22 with no log line,
  so `m4-gpu.service` fails and SDDM, which requires it, never starts.
  This matches the #57 symptom. The cause on that Mac was not measured.

Two fork PRs came out of this. **Fork PR #1** (`feat/j713-gpu-preflight`)
adds a read-only macOS preflight, `tools/m4-j713-gpu-preflight.py`. It repeats
m1n1's grouping, aborts and power model from `ioreg` and checks the module's
four constants before partitioning. On this Air it exits 3 (GPU will fail):
10 groups, top 1470 MHz at 995 mV, `maximum power 24676 mW, module requires
24800`. Its power model matched an independent C copy of m1n1's
`m4_power_mw` on 640,142 cases with no differences. **Fork PR #2**
(`fix/j713-gpu-start-diagnostics`) sends `m4-gpu-start` output to the
journal under `bash -x`. On failure it also prints the three loader
properties and the G16 kernel lines. It has not been deployed on this Air.

A comment for #57 explaining the gate was drafted. It has not been posted.

## Installation

First, macOS automatic updates were turned off; all four Software Update
settings, including security responses, read 0 afterwards. A macOS update
would change the AudioDSP again and move the system firmware further from
anything tested. No Time Machine backup existed, so the owner's personal
folders (about 11 GB, 141,159 files) were copied to another machine and
counted. Then, in the kit's `installer/` folder, at the Mac's own Terminal
(reusing the downloaded IPSW):

```sh
bash Install.command --install --ipsw-dir ~/Downloads/ipsw
```

| Prompt | Answer |
| --- | --- |
| Continue after START-HERE; Widevine | `y`; `N`. Speaker filters then passed with the patched kit |
| Expert mode | `y` (required for J713) |
| Main menu (no free space) | `r` to resize partition 1 |
| "Overhead: 20.12 GB … Continue anyway?" | `y`. The warning is false here: no local snapshots existed |
| New macOS size | `340GB`, leaving 154.38 GB free |
| Main menu | `f`, then Omarchy M4 Native, size `max`, name `Omarchy M4` |
| Boot firmware version | 26.5.1, the only choice |

Step 2 followed the kit's instructions: press Enter, wait at least 25
seconds, hold the power button, choose **Omarchy M4**, then authenticate
twice (`bputil -nc`, then `kmutil configure-boot`). The installer log on the
ESP had no errors. The resulting disk layout:

| Partition | Contents |
| --- | --- |
| 1 | Apple system support (iSCPreboot) |
| 2 | macOS APFS, 340 GB |
| 3 | Omarchy M4 stub, 2.5 GB APFS |
| 4 | ESP, 524 MB FAT32 (`EFI - OMARC`), starting at 4 KiB block 83,746,310 |
| 5 | Linux root, 151 GB (ext4 image expanded on first boot) |
| 6 | 134 MB `Apple_Boot`, created by macOS during partitioning; harmless |
| 7 | Recovery |

## The boot hang

### Symptom

After step 2 the Mac rebooted into Omarchy M4 and showed the Apple logo, then
the m1n1 (Asahi) logo, and stayed there: no text, flicker or backlight change
for more than 10 minutes, and neither Caps Lock nor Ctrl+Option+F2 did
anything. A second boot after a forced power-off was identical after 3
minutes. With `console=tty0 fbcon=nodefer` on the command line, a running
kernel would have replaced the logo with text, so this pointed below Linux
(inferred).

### Ruled out from macOS

Each check here was read-only.

- **Boot policy:** `bputil -d -v <Omarchy-VGID>` showed Permissive security,
  a registered custom kernel and Kernel CTRR disabled, as expected.
- **ESP contents:** `diskutil mount readOnly` refused the ESP even with
  `sudo`, giving no reason, so a raw `dd` copy was read on Linux with
  `fsck -n`. `omarchy/boot.bin` is the release `loader.bin` (`3a3f7a64…`,
  0x110000 bytes), then the `chosen.bootargs` line and the release
  `Image.gz`, `portable.dtb` and initramfs, each byte-identical. The
  stage-1 vars name the ESP and root partitions by PARTUUID, matching the
  disk.
- **Root filesystem:** the first 4 MiB of partition 5 showed an ext4
  superblock still at 16 GiB, last written on the build machine, clean with
  0 errors. Linux had never mounted its root, so the forced power-off did
  no harm.

### Stage-2 experiments through the ESP

The ESP image was backed up on macOS first; test images were written with a
guarded `dd` that checks the target partition and the image checksums. A USB-C
data cable from the Air let a Linux host watch for the m1n1 proxy
(`1209:316d`). Only the boot picker's own USB device ever appeared.

| ESP `boot.bin` variant | Expected if stage 2 runs | Observed |
| --- | --- | --- |
| Loader only (kernel removed) | "No valid payload found", then the USB proxy | Logo only; no proxy |
| Stage-2 logo replaced by a green square | Green square | Asahi logo only. The ESP checksum read back afterwards confirmed the image was in place |
| Probe A: stage-2 progress blocks and register readouts | White blocks along the top | No blocks; no proxy |

**Observed:** stage 2 never drew anything. Stage 1 draws the logo that stayed
on screen. Stage 2 copies that framebuffer and re-blits a logo at the same
position, so the screen alone cannot separate a hang late in stage 1 from one
early in stage 2. The green logo settled it: the hang was in stage 1. Stage 1
lives in the stub, not on the ESP, so ESP changes cannot reach it.

### Static analysis

Against upstream AsahiLinux/m1n1 at `c9ef56f` (v1.6.1-22), the fork changes
only `kboot_gpu.c`, adds `m4_power.h` and an unused `#ifdef` in `wdt.c`.
Everything from `_start` to the payload jump is unmodified upstream code in
both stages. Stage 1 is built `RELEASE + CHAINLOADING`, so `FB_SILENT_MODE`
is on. The unbounded waits that can hang without output are the UART and
dockchannel TX busy-waits in the console path, the ANS RTKit power-on and
sleep handshakes, and any fault before `wdt_disable`, which ends in a silent
`b .`. The J713 ADT node `/arm-io/dockchannel-uart` has `enable-sw-drain = 1`
and `dock-wstat-mask = 0xfff`, so its TX FIFO holds at most 4 KiB
(`DATA_TX_FREE` at `0x3_8812_C014`). m1n1 prints several KiB per stage.

### Verbose boot attempts

First `sudo nvram boot-args=-v` from macOS (it read back correctly), then
`csrutil disable` from Recovery, choosing **Omarchy M4** rather than
Macintosh HD, followed by `nvram boot-args=-v` there. Both times Omarchy M4
showed only the logo and no USB proxy. Afterwards the stub's policy showed
SIP off (`sip0` = 127, the value stage 1 tests for), but `nvram boot-args`
was **empty** when read back from macOS.

Stage 1 opens a 5-second USB proxy window before touching NVMe only when
both `video.display == 0` (from `-v`) and `lp-sip0 == 127`. **Inferred:**
`-v` never reached the stub, so that window never opened.

### Debug stage 1 builds

The stub's stage-1 object, copied from `/Volumes/Omarchy M4/Finish
Installation.app/Contents/Resources/boot.bin`, is the kit's `boot/m1n1.bin`
(0x120000 bytes) plus a 64 KiB vars block holding the `chainload=` and root
identity lines. Debug builds were binary patches of that file:

- **Placement:** a position-independent blob of 768–1,152 bytes in the dead
  `nop` padding at file offset `0x5900–0x6000`, assembled with LLVM and
  linked with `lld` (`-flavor gnu --image-base=0 -Ttext=0x5900
  --no-rosegment`). Converting an unlinked object directly produced branches
  to themselves, so linking is required.
- **Hooks:** `_start` (`0x800`, `mov x19,x0`) branches to an entry hook that
  stashes the framebuffer base and stride and paints block 0 plus register
  rows. Each marked `bl callee` goes through a trampoline that paints block
  *k*, then tail-calls the callee. `nvme_read` is hooked at its entry.
- **Geometry:** blocks are 28×32 px at x = 96 + 40·k, y = 16–47: above the
  console region (y ≥ 64) that `fb_init()` clears in its shadow buffer even
  with `clear=false`, and left of the notch band. Register rows sit at
  x ≥ 1536, sixteen cells each, MSB first, tall = 1, short = 0. Values were
  read from phone photos.
- **Builder checks:** every patch site's original word, an untouched `nop`
  pad and a byte-identical vars tail at `0x120000`.

Main block map:

| Block | Painted before |
| --- | --- |
| 0–4 | `_start` (plus register rows), `dockchannel_uart_init`, `get_device_info`, `init_cpu`, `m1n1_main` |
| 5–15 | `firmware_init` … `wdt_disable`, `pmgr_init`, display, `fb_init`, `fb_display_logo` (block 15 is overwritten by the logo blit) |
| 16–22 | `cpufreq_fixup`, `sep_init`, `run_actions`, `payload_run`, `chainload_load`, `nvme_init`, `rust_load_image` |
| 23 | `nvme_read` entry (first GPT read and every FAT read) |
| 24 | final jump into stage 2 |

Later builds added rows for the `nvme_read` call count, which is kept in
`TPIDRRO_EL0`, and for the LBA. They also added phase hooks:

| Phase | Hook site | Meaning when shown |
| --- | --- | --- |
| 1 | `bl nvme_shutdown` at `0x9f7c` | `rust_load_image` returned; its return value is shown on its own row |
| 2 | `bl rtkit_sleep` at `0x23dbc` | about to put ANS to sleep |
| 3 | first `bl pmgr_reset` at `0x23dd0` | `rtkit_sleep` returned |
| 4 | second `bl pmgr_reset` at `0x23de0` | first ANS reset returned |
| 5 | `bl rtkit_free` at `0x23de8` | both resets returned |
| 6 | `bl chainload_image` at `0x9f94` | NVMe shut down, about to build stage 2 |

Every build was installed from the stub's paired 1TR with the same command the
installer's step 2 uses (see [Recovery gotchas](#recovery-gotchas)).

| Build | Observed |
| --- | --- |
| Markers | Blocks 0–14, the logo, then blocks 16–23 ("15 then 8"). No block 24. `DATA_TX_FREE` was non-zero at stage-1 entry (roughly 1–2 KiB free in the photo decode) |
| Markers, `rtkit_sleep` call skipped | Same blocks. After a few minutes the Mac restarted, then boot-looped every 5–10 s until it showed "macOS needs to be reinstalled". The reset cause was not investigated |
| Read counter and phases 1–3 | Eleven photos over time, all identical: **712** `nvme_read` calls, last LBA `0x04FE4E20` (28,698 blocks into the ESP), `rust_load_image` returned **0**, phase **3**. Frozen, not slow |
| Phases 1–6, dockchannel disabled | Stage 2 (probe A, still on the ESP) ran, parsed the stage-1 vars, printed "No valid payload found" and opened the USB proxy |
| Phases 1–6, dockchannel enabled | Built as the control; not needed and not run |

How these results were read:

- **The non-zero `DATA_TX_FREE` first seemed to rule out a full FIFO.** It
  only shows that the FIFO had room at entry; stage 1 prints throughout and
  nothing drained it.
- **The read-counter run moved the hang past the I/O.** The whole 1.44 MB
  probe-A `boot.bin` was read (the count matched the estimate for that
  file), the loader succeeded and ANS went to sleep.
- **Pinpointing the statement (inferred).** Phase 3 means `rtkit_sleep`
  returned and the first `pmgr_reset` began. The next statement in
  `pmgr_reset_device()` that can block is `printf("pmgr: resetting device
  %d.%s\n", …)`. With only `dockchannel_uart_init` changed, the same build
  ran through to stage 2.

### USB chainload to Linux

With stage 2 in the proxy, the rest of the boot was tested without Recovery:

1. **Proxyclient.** The release's proxyclient matched upstream m1n1's
   `c9ef56f` with zero differing files. The upstream copy was used.
2. **Payload.** The fixed release `boot.bin` (see below) plus the three
   stage-1 vars lines, in m1n1's appended-vars format:

   ```text
   chosen.gravity,efi-system-partition=<ESP-PARTUUID>
   chosen.omarchy,root-partition=<ROOT-PARTUUID>
   chosen.omarchy,root-filesystem=<ROOT-FS-UUID>
   ```

3. **Load.** From the proxyclient checkout on the Linux host:

   ```sh
   USE_CLANG=1 TOOLCHAIN=/usr/bin/ LLDDIR=<dir-with-ld.lld> M1N1DEVICE=/dev/ttyACM0 \
     python tools/chainload.py -r chainload-payload.bin
   ```

   `chainload.py` assembles a small stub; an `ld.lld` symlink to rustup's
   `rust-lld` was enough as its linker.

m1n1 printed `Preparing to run next stage` and the USB link dropped. The
owner then saw Omarchy's first-boot setup ("press Return to start setup").
Setup created the account and expanded the root filesystem to 139 GB. As
predicted, the desktop failed: `m4-gpu.service` failed with the -22
fingerprint, so login was on tty2 (Ctrl+Option+F2).

### Recovery gotchas

Every debug stage 1 was installed the way the installer's step 2 installs the
original, from the stub's paired 1TR (hold power → Options → Terminal), with
the file staged on the macOS Data volume beforehand:

```sh
bputil -d -v <Omarchy-VGID> | grep Pairing     # must print: Paired
F=/Volumes/Data/Users/<macos-user>/omarchy-debug/<build>.bin
kmutil configure-boot -c "$F" --raw --entry-point 2048 \
  --lowest-virtual-address 0 -v "/Volumes/Omarchy M4"
```

`kmutil` asks for confirmation and credentials, then prints `installing boot
object… done`. Three things went wrong along the way:

- **"Boot policy error: … code pairing (17)".** Options opens the 1TR paired
  with the *current default* startup disk. After the reinstall screen below
  that was macOS, and `sw_vers` showed 26.6.2 in both 1TRs. Fix: in that
  Terminal, `bless --setBoot --mount "/Volumes/Omarchy M4"`, shut down,
  Options again, and check `Paired`. Omarchy M4 then stays the default;
  hold power and choose Macintosh HD to reach macOS.
- **"macOS needs to be reinstalled".** It appeared after the no-sleep build
  boot-looped and concerns the stub, not macOS. Choose Startup Disk… →
  Macintosh HD → Restart; nothing needs reinstalling. This changes the
  default startup disk, which causes the pairing error above.
- **boot-args do not persist.** `nvram boot-args=-v`, set from macOS (read
  back correctly) and again from Recovery, was empty when checked later.
  SIP changes made with `csrutil` for the stub did persist.

## Root cause and fix

### Mechanism

`dockchannel_uart_putbyte()` in upstream m1n1, unmodified in this fork:

```c
void dockchannel_uart_putbyte(u8 c)
{
    if (!uart_base)
        return;
    while (read32(uart_base + DATA_TX_FREE) == 0)
        ;
    write32(uart_base + DATA_TX8, c);
}
```

**Observed:** `dockchannel_uart_init()` finds `/arm-io/dockchannel-uart` in the
ADT on this Air and registers the iodev. With it active, stage 1 froze after
phase 3; with it returning -1, the same build reached stage 2.
**Inferred from the code:** stage 1 printed until the at-most-4 KiB TX FIFO
was full, nothing drained it, and the next byte spun forever in the loop
above. Console writes and flushes go to the devices in order UART (0),
dockchannel (1), framebuffer (2), so a line blocked at the dockchannel never
reaches the framebuffer, which in stage 1 is silent anyway unless `-v` takes
effect. That explains why the hang showed nothing.

Why the maintainer's Air and the #57 reporter's Air do not hang is
**unknown**: something must drain the FIFO there, or it starts emptier. The
ADT's `enable-sw-drain = 1` suggests macOS drains it in software (a reading,
not verified). This Air's newer system firmware is a candidate difference,
also unverified.

### Fix

Make `dockchannel_uart_init()` return -1 at entry. `uart_base` stays 0, the
iodev never becomes usable, and every put returns immediately. This is
equivalent to removing the `dockchannel_uart_init()` call in `startup.c`. The
only loss is dockchannel debug output, which nothing reads on this Air.

```text
dockchannel_uart_init, first two instructions (little-endian words)

Stage 1  (stub object / kit installer payload boot/m1n1.bin)
  0xf27c  d10103ff  sub  sp, sp, #0x40           ->  12800000  mov  w0, #-1
  0xf280  a9027bfd  stp  x29, x30, [sp, #0x20]   ->  d65f03c0  ret

Stage 2  (loader.bin = boot.bin[0x0:0x110000]; same file offsets in boot.bin)
  0xe80c  d10103ff  sub  sp, sp, #0x40           ->  12800000  mov  w0, #-1
  0xe810  a9027bfd  stp  x29, x30, [sp, #0x20]   ->  d65f03c0  ret

File bytes at each site: ff 03 01 d1 fd 7b 02 a9  ->  00 00 80 12 c0 03 5f d6
```

The equivalent script, which refuses unless the original words are present:

```python
import struct, sys
src, dst, *sites = sys.argv[1:]          # e.g. stage1-boot.bin stage1-nodock.bin 0xf27c
b = bytearray(open(src, 'rb').read())
for site in sites:
    off = int(site, 16)
    for o, old, new in ((off, 0xd10103ff, 0x12800000), (off + 4, 0xa9027bfd, 0xd65f03c0)):
        assert struct.unpack_from('<I', b, o)[0] == old, hex(o)
        struct.pack_into('<I', b, o, new)
open(dst, 'wb').write(b)
```

**Stage 2** runs the same code and was patched as a precaution; it was **not**
shown to hang on its own. Once stage 1 was fixed, the dockchannel-enabled
probe-A stage 2 reached the proxy, but it prints far less than a kernel boot.

| Artifact | SHA-256 |
| --- | --- |
| Kit `boot/m1n1.bin`, stage 1 code, original | `18cc4cd1ba07928078d2cb52a2c1c5f40a4661fd3db7ee6af0e6923fd237be10` |
| Same, patched (= first 0x120000 bytes of the fixed stub object) | `15ef246ac297304f559f334cb36f3c4c9cfb674487ccae65633014649b469848` |
| Release `loader.bin`, original | `3a3f7a64c65fc366d5ac9aece3e1bee64119be012aa1ec5b15d2743c6f91fce4` |
| `loader.bin`, patched | `97550b7c505879ad8c2b8e79618b4f0581e10d2e3eb6043182016882d0a0407e` |
| ESP `omarchy/boot.bin`, as installed | `7ffe8d3ab16505e87637dbeeb65177c9518838c993e1987e67c74b041a6f05ea` |
| ESP `omarchy/boot.bin`, fixed (installed now) | `10f21f0e24b6a864a4ea8fe0b4eec1df9b2b7ac28b46175b61a33ae83d1a8262` |

The installed `boot.bin` held only release components, so both `boot.bin`
hashes should match on any alpha.1 install (inferred, not checked on a second
machine). Stub objects carry each install's partition identifiers in the vars
tail; compare their first 0x120000 bytes instead.

### Applying it

- **ESP:** from a booted Linux, copy the fixed file and check its hash:

  ```sh
  sudo install -m 700 boot-nodock.bin /boot/efi/omarchy/boot.bin && sync
  sudo sha256sum /boot/efi/omarchy/boot.bin
  ```

  Before Linux boots, use the USB chainload above, or write a whole ESP image
  from macOS with a guarded `dd` (`diskutil` would not mount the ESP here).
- **Stage 1:** patch a copy of the installed stub object at `0xf27c` and
  install it with the `kmutil` command in
  [Recovery gotchas](#recovery-gotchas). Its `-v` is the stub System volume
  path, not a VGID. On this Air the stub still runs the debug
  `stage1-phase-nodock.bin` (same 8 bytes plus markers); the clean
  `stage1-nodock.bin` is built but **not yet installed**.

### Proper fix

The real fix belongs in upstream m1n1: a bounded wait (timeout) in
`dockchannel_uart_putbyte()`, or a free-space check that drops output when the
FIFO stays full, possibly honouring `enable-sw-drain`. The RTKit power-state
loops have the same unbounded shape. This has **not yet been reported** to
AsahiLinux/m1n1 or to the Omarchy M4 maintainer.

## The GPU fix

### Patch

One byte in `asahi_m4.ko`. The comparison is at `.text+0x224c0`, which is file
offset `0x22500` (`.text` starts at file offset `0x40`):

```text
224b8: 528c1c09  mov   w9, #0x60e0        // 24800
224bc: 6b09011f  cmp   w8, w9             // w8 = apple,m4-max-power-mw
224c0: 54004101  b.ne  0x22ce0            // -> -22
                 becomes
224c0: 54004108  b.hi  0x22ce0            // reject only above 24800
```

| Module | SHA-256 |
| --- | --- |
| Shipped `asahi_m4.ko` | `80cf3669575edd09aed226a93e47aaaf132e70f1070154629c413c0b40d34578` |
| Patched | `ca519bda6fa29b99b7f87944b64c5c5d3bf2a850b4604ba17b2d3be56893b0d2` |

In the shipped binary, 24800 appears only in this comparison. After the gate,
the published G16 code reads the DT value, range-checks it and uses it as
the firmware power-controller budget (`g16_platform.rs`, `g16_power.rs`).
With `b.hi`, this chip's firmware gets its own 24,676 mW budget, slightly
below the maintainer's. That the value has no other effect is **inferred**
from code reading; it is not qualification.

`apply-gpu-power-gate.sh --apply` checks the running kernel
(`7.1.9-m4-gpu-backport-20261008`), the derived power (1..24800) and both
module hashes. It keeps `asahi_m4.ko.orig` and `runtime.sha256.orig`, installs
the patched module and rewrites its line in `runtime.sha256`. `--revert`
restores both originals. `m4-gpu-start` verifies the module only
against `runtime.sha256`. Module signatures are not enforced
(`CONFIG_MODULE_SIG=y`, not forced), so the kernel only taints.

### Evidence

The first unassisted boot showed the predicted fingerprint (log prefixes
trimmed), and `od -An -tu4 --endian=big
/proc/device-tree/soc/gpu*/apple,m4-max-power-mw` printed `24676`:

```text
asahi_m4 302600000.gpu: G16: boot memory contract validated; firmware initialization pending
asahi_m4 302600000.gpu: probe with driver asahi_m4 failed with error -22
```

The failed module was then removed and the patched one loaded once from
`/tmp`, before anything was made permanent:

```text
G16: GPU ID=0x0a021100 revision=0x11
G16: autonomous RTKit boot complete; firmware endpoints ready
G16: published Linux-built initialization graph
G16: firmware accepted initialization graph
G16: opening control registration retired; cursors (3, 3, 3)
G16: barrier channel retired; two items consumed
[drm] Initialized asahi 0.0.0 for 302600000.gpu on minor 1
```

The same `eglinfo -B -p gbm` check that `m4-gpu-start` runs:

```text
OpenGL core profile renderer: Apple M4 (G16G B1)
OpenGL core profile version: 3.3 (Core Profile) Mesa 26.3.0-devel (git-87ac51d411)
OpenGL ES profile version: OpenGL ES 3.0 Mesa 26.3.0-devel (git-87ac51d411)
```

After `--apply` and a reboot, `m4-gpu.service` logged `M4_GPU_AUTOSTART_READY`,
no units failed, and the Air reached the graphical login. `m4-video.service`,
which had failed with the GPU, also started.

### Crash under sustained load (cause unknown)

- **The load:** an offscreen GLES fragment-shader bench (2560×1600,
  128-iteration loop, `glFinish` every frame, about 87 fps) with
  `es2gears` on screen. Clear readback was exact (`64 128 191 255`), two
  identical draws gave identical checksums, and an earlier 4-second 1080p
  run at 64 iterations passed at 302 fps.
- **The crash:** at 02:28:34, about 20 s into a planned 55-second run
  (log prefixes trimmed):

  ```text
  asahi_m4 302600000.gpu: RTKit: co-processor has crashed
  RTKit: Message (id=1): agfMTRAlarmHandler: !!! Fatal Error: Received MTR Alarm interrupt from GPU but no match found in MTR hardware! chainID=0
  G16: firmware crashed
  asahi_m4: G16: submission pipeline failed: EIO, 1 jobs, 1 issued
  ```

- **What followed:** every GPU client got EIO, including the bench. Hyprland
  aborted, restarted, then died on `DRM_IOCTL_ASAHI_VM_BIND failed: EIO`; the
  portal crashed, the user session ended and the SDDM greeter crash-looped
  every 15 s. The driver has no firmware-restart path; a reboot restored
  graphics normally.
- **Temperatures at the crash:** battery hotspot 35 °C, charge regulator
  51 °C, Wi-Fi 58 °C. No SoC or GPU die sensor is exposed.

**The cause is unknown.** The firmware runs with a 24,676 mW budget the
module was never qualified with, which makes the patch the first suspect, but
nothing ties the MTR alarm to the power budget; the alpha driver or firmware
may fail under any sustained load. It was seen once, not reproduced, and has
no 24,800 mW comparison. Until it is understood, avoid long heavy GPU work
(games, benchmarks, WebGL demos). Normal desktop use and short loads were fine.

## Deep test results

Three test passes ran concurrently on October 14, 02:08–02:30, over SSH:
graphics and video, peripherals and power, and system health. They were
read-only apart from the stated tests; nothing was installed and suspend was
not attempted. The passes overlapped, so CPU stress and `mpv` playback shared
the machine with the audio and GPU tests. Status: **works**, **partial**,
**broken** or **untested**.

### Graphics and video

| Feature | Status | Evidence / notes |
| --- | --- | --- |
| GPU driver (patched) | works | Firmware accepts init graph; `M4_GPU_AUTOSTART_READY` |
| Compositor rendering | works | Hyprland renders on the GPU (`Renderer: Apple M4 (G16G B1)`), imported into simpledrm, which CPU-copies to the 10-bit firmware framebuffer |
| API level | partial | GL 3.3 / GLES 3.0. GLES 3.1 and GL 4.x contexts fail; no Vulkan ICD, so no WebGPU |
| Sustained GPU load | **broken** | Firmware crash (above) |
| Display controller | broken | Only simpledrm; no DCP. One 2560×1600 mode on a 2560×1664 panel; no EDID, bogus physical size |
| Frame pacing | partial | 64–71 FPS with vsync against a 60 Hz panel; no real vblank. Tearing not checked by eye |
| Brightness | broken | `/sys/class/backlight` empty; F1/F2 do nothing |
| Night light / colour | broken | No CTM or gamma property |
| DPMS | partial | Compositor off/on works; whether the backlight turns off is unverified |
| External display | broken | No DP alt-mode or DCP-ext driver |
| Screenshots (`grim`) | works | Blocks while DPMS is off |
| GTK4, Qt, quickshell | works | GLES 3.0 through `/opt/m4-mesa` |
| X11 apps (Xwayland) | broken | Software only. `/opt/m4-mesa` ships a non-glvnd `libEGL`, so direct `libGL`/`libOpenGL` calls return NULL and glamor fails |
| System Mesa 26.2.4 | broken | `DRM_IOCTL_ASAHI_QUEUE_CREATE failed: Invalid argument`; any process that loses `LD_LIBRARY_PATH` falls back to it or llvmpipe |
| Brave, default launch | broken → fixed | Ran under Xwayland on llvmpipe. With `--ozone-platform=wayland`: GPU compositing, WebGL 1/2 on Apple M4, rAF 59.9 fps |
| AVD decoder | works | `M4_VIDEO_READY: H264 and VP9 hardware decoder registered` |
| VA-API H.264, VP9 (ffmpeg) | works | 1080p 203–235 fps (H.264) and 178 fps (VP9); 4K 59/47 fps; 60 frames bit-identical to software decode |
| HEVC, AV1 hardware | broken | Not exposed by the VA driver. HEVC falls back to software; ffmpeg AV1 via VA-API fails (dav1d works); HEVC does not play in Brave |
| mpv | partial | 1080p30: 0–1 drops in 4.5 s. 4K30: 32–81 drops in 4.5 s with both hardware and software decode; only 4 with `--vo=null` |

### Audio and input

| Feature | Status | Evidence / notes |
| --- | --- | --- |
| Speaker chain | partial | Four TAS2764/SN012776 amps bound; `speakersafetyd-j713` active, gain ramped to nominal; FIR graph hashes match the manifest. A −30 dBFS 1 kHz tone played through the protected sink with `faulted=0`. **Audibility unverified by ear** |
| Internal microphones | broken | AOP node disabled; `CONFIG_APPLE_AOP` unset; no PipeWire source |
| Headphone jack | broken | No `cs42l84` node in the DT |
| Keyboard | works | Apple MTP keyboard, MTP firmware `AppleMTPFirmwareMac-5340.61.4`, `hid_apple fnmode=3` |
| Touchpad | works (driver) | `Touch MT ready` this boot; gestures, haptics and palm rejection not checked by hand |
| Keyboard backlight | works (PWM) | Writing 80 gave PWM `12542/40000 ns`; lit keys not confirmed by eye |
| F-row mapping | untested | Capability bitmap present |
| Touch ID | broken | SEP disabled. The button acts as the power key, which opens the system menu |

The card also offers a `pro-audio` profile. Selecting it would bypass the FIR
crossover, so tweeters would get full-band audio. Do not select it.

### Wireless, power and CPU

| Feature | Status | Evidence / notes |
| --- | --- | --- |
| Wi-Fi (USI BCM4388 C2) | works | First boot of the USI module under this project. Firmware `23.50.20.0.41.51.208`; 2.4 GHz HT20 MCS15, about 95/93 Mbit/s over the LAN |
| Wi-Fi 5/6 GHz | untested | Bands enabled; no 5/6 GHz AP in range; regulatory domain was world `00` (fixed below) |
| Wi-Fi Direct / P2P | broken | Firmware rejects the iovars (`-52`); station mode unaffected |
| Bluetooth | works | Controller powered; a 10 s LE scan found 3 devices. Pairing and audio untested |
| Battery, AC, MagSafe | works | 100 %, health Good, 9 cycles, 51.94 of 52.77 Wh; 34.8 W input limit |
| Charge limit controls | present | Not exercised |
| Power profiles | partial | `placeholder` driver only |
| Idle power | untested | Needs an unplugged run |
| Suspend (s2idle) | untested | Deliberately not tried. No cpuidle. Speaker protection has `Restart=no` and latches faults. Hidden from the menu (below) |
| Shutdown | untested | `macsmc-reboot` logs the missing NVMEM cells of alpha.1 ([issue #24](https://github.com/Kb2uka/Omarchy-M4-Native/issues/24), [poweroff fix](j713-poweroff.md)); the M4 update channel is not connected. Restart via `systemctl reboot` worked several times |
| CPU topology | works | 10 CPUs online: cpu0 and cpu7–9 P (`donan-p`), cpu1–6 E (`donan-e`) |
| CPU frequency scaling | **broken** | `cpufreq/` empty; the t8132 DT has no OPP or cluster-cpufreq nodes (`CONFIG_ARM_APPLE_SOC_CPUFREQ=m` never loads). Dependent-add timing: P ≈ 1.6–1.8 GHz, E ≈ 0.8–0.9 GHz. `openssl speed sha256`: 1.35 GB/s on a P-core |
| CPU idle / capacity | broken | No cpuidle driver (WFI only); `cpu_capacity` 1024 on all CPUs, so the scheduler treats E as P |
| Thermals | partial | Only SMC battery, charger, Wi-Fi and "Heatpipe Power" sensors; no SoC, CPU or GPU die temperature |
| Lid switch | partial → fixed | Kernel `SW_LID` works. Userspace ignored it (below) |
| USB-C | partial | Left-front data controller disabled in the DT (known); Linux saw a non-PD partner there, most likely the debug cable, with no enumeration. The hinge-side port was not tested with a device under Linux. m1n1's proxy initialized both controllers |
| Camera, ambient light | broken | No ISP node; ALS sits under the disabled AOP |

### System, storage and updates

| Area | Status | Evidence / notes |
| --- | --- | --- |
| Failed units, oopses | ok | 0 failed units; no Oops, WARN, hung task or RCU stall in three boots; no deferred probes |
| Boot time | ok | 0.8 s kernel + 19.3 s userspace. `speakersafetyd-j713` readiness takes 15.7 s, but SDDM is up at about 5 s |
| Kernel command line | warning | Debug options baked into `boot.bin`: `m4trace_hit=999` (146 warning lines per boot), `panic=0` (hang on panic), `pcie_apple.m4_keep_clocks=1`; `idle=nop` is ignored on arm64 |
| Taint, BTF | info | Taint S+O+E; no `/sys/kernel/btf/vmlinux`, so systemd `RestrictFileSystems=` is not enforced |
| NVMe health | ok | 0 media errors; 0 % used; 20 unsafe shutdowns, presumably the forced power-offs while debugging |
| Storage throughput | ok | Direct I/O: 2.0 GB/s write, 2.1 GB/s read; 4 KiB `O_DSYNC` 22.4 MB/s |
| Root filesystem | ok | ext4, 140.8 GB, clean, `metadata_csum` |
| TRIM | fixed | `fstrim.timer` was disabled |
| CPU stress | ok | 60 s, 10 threads with NEON FMA and a memory walk; no throughput decline. At these clocks this is not a full-power thermal test |
| Memory and I/O stress | ok | 4 GiB × 3 passes with 0 mismatches; 4 × 512 MiB direct write/read with sha256 verify, 4/4 matched |
| Firewall | warning | `ufw` inactive and disabled; this image never enabled it |
| Kernel package updates | risk | `linux-m4-omarchy` ships modules and `Image.gz` only. Nothing rebuilds `boot.bin`, and `m4-gpu-start` pins `uname -r`. A new package version would leave `boot.bin` booting the old kernel without its modules (inferred from package contents) |
| `aquamarine` | risk | Installed 0.15.1-1.1 carries the M4 simpledrm GBM path (`AQ_M4_SIMPLEDRM_GBM`). An ALARM rebuild would replace it, likely with no desktop, and the package cache is empty |
| `omarchy update` | risk | No snapshot (ext4). It moves conflicting unowned files to `/var/lib/omarchy/replaced`. It always prompts "Linux kernel has been updated" (false). An AUR item is x86-only |
| `omarchy-refresh-pacman` | broken if run | Installs the x86 `pacman.conf` and the `[omarchy]` aarch64 repository, whose index carries the M1/M2 boot packages. Never run it on this machine |
| Pending updates | info | 122 packages; a dry run resolves cleanly with the pins below |

## Post-install fixes applied

Before any change, an archive of the M4-specific files no package owns was
copied off the machine with the ESP `boot.bin`: `/opt/m4-*`, the M4 units,
`/usr/lib/omarchy`, the speaker-protection configs, the Apple firmware under
`/usr/lib/firmware`, `/etc/pacman.conf` and the files of the installed
`aquamarine`, `hyprland`, `hyprtoolkit` and `hyprland-guiutils` packages.

| Fix | Change | Result |
| --- | --- | --- |
| Pin the M4 packages | `IgnorePkg = linux-m4-omarchy aquamarine` in `/etc/pacman.conf`; original kept as `pacman.conf.pre-m4pin` | Applied. Update with `sudo env OMARCHY_ALLOW_DIRECT_PACMAN=1 pacman -Syu` after reading the list, not "Update System". If pacman refuses because of `aquamarine`, stop |
| Brave on Wayland | `~/.config/brave-flags.conf` → symlink to Omarchy's `chromium-flags.conf`, which has `--ozone-platform=wayland` | The owner reported Brave working; `brave://gpu` was not recorded |
| Hide Suspend | `omarchy-toggle-suspend` (the `suspend-off` toggle) | Suspend no longer offered in the system menu |
| TRIM | `sudo systemctl enable --now fstrim.timer` | Enabled |
| Regulatory domain | Uncommented `WIRELESS_REGDOM="<CC>"` in `/etc/conf.d/wireless-regdom`; `iw reg set <CC>` | `iw reg get` shows the country instead of world `00`. Before, the boot log had `set-wireless-regdom failed` with no country set; not rechecked after a reboot |
| Lock-screen PAM | Ran `omarchy-apply-lock` | Created `/etc/pam.d/omarchy-lock-password`; the lock now reports `passwordPam: true`. Before, the shell's lock refused with `missing-pam`, so idle locking could not have worked either (inferred). The M4 installer never ran this Omarchy step. Polkit was unaffected (`/usr/lib/pam.d/polkit-1`) |
| Lid close/open | Two bindings in `~/.config/hypr/bindings.lua` (original kept as `bindings.lua.pre-lid`) | Closing locks; opening turns the display back on. Confirmed by the owner |

The lid needed both the PAM file and new bindings. Omarchy binds
`switch:on:Lid Switch`, but this Air exposes the switch as **`Apple SMC
power/lid events`**. `omarchy-hw-laptop-closed` reads `/proc/acpi/button/lid`,
which does not exist on arm64; `omarchy-hyprland-monitor-laptop` looks for
`eDP-*`, but the panel is `Unknown-1`; and every logind `Handle*` is `ignore`.
After the first binding, Hyprland's log showed the switch firing the bind. The
session then locked (`locked: true`, `secure: true`), but on reopening the
display stayed off (`dpmsStatus: 0`); `omarchy-system-lock` turns DPMS off. The
old `hyprctl dispatch dpms on` syntax fails under the Lua config with
`')' expected near 'on'`. The final bindings:

```lua
-- M4 Air: the lid switch is "Apple SMC power/lid events", not "Lid Switch",
-- and omarchy-hw-laptop-closed reads /proc/acpi, which does not exist here.
o.bind("switch:on:Apple SMC power/lid events", nil, "omarchy-system-lock", { locked = true })
o.bind("switch:off:Apple SMC power/lid events", nil, "hyprctl dispatch 'hl.dsp.dpms({ action = \"enable\" })'", { locked = true })
```

A closed Air still does not suspend. It only locks.

## Current state

| Component | State |
| --- | --- |
| Stub stage 1 | Debug `stage1-phase-nodock.bin`: the dockchannel fix plus progress markers, which stay visible at the top during boot. Clean `stage1-nodock.bin` prepared, not installed |
| ESP `omarchy/boot.bin` | Fixed, `10f21f0e…`. Boots unassisted |
| GPU module | Patched, `ca519bda…`; originals kept as `.orig` |
| Startup disk | Omarchy M4 is the default. Hold the power button and choose Macintosh HD for macOS |
| Stub boot policy | Permissive, with SIP disabled for the stub (`sip0` `0x7f`) from the debugging. boot-args may still hold `-v` |
| macOS | 26.6.2, untouched apart from the resize; automatic updates off |
| Linux | Kernel `7.1.9-m4-gpu-backport-20261008` #18; Omarchy 4.0.4; the M4 update channel is not connected |
| Remote access | `sshd` enabled for testing, with password authentication on; temporary debugging access still in place |

## Known issues and next steps

1. **Install the clean stage 1** (`stage1-nodock.bin`) from the stub's paired
   1TR to remove the marker blocks, and confirm it boots.
2. **CPU DVFS.** The `boot.bin` devicetree needs t8132 cluster-cpufreq nodes,
   OPP tables and `capacity-dmips-mhz`: maintainer DT work, and the largest
   performance gap (about 40 % of P-core peak). Repeat the thermal stress
   test once clocks scale.
3. **GPU stress crash.** Reproduce with a stepped load (duration ×
   resolution), log SMC temperatures, compare against a 24,800 mW Air if
   possible, and report with the logs. Avoid sustained GPU loads until then.
4. **Clean up debugging state.** Remove the temporary remote-administration
   access, enable `ufw` with Omarchy's defaults, disable SSH password logins
   or stop `sshd`, and delete the debug images. On the macOS side,
   optionally run `csrutil enable` and `nvram -d boot-args` for the stub
   from its paired 1TR, and choose the default startup disk.
5. **Upstream reports**, each only with the owner's approval:
   - the m1n1 dockchannel hang, to AsahiLinux/m1n1 and the maintainer, with
     the offsets and the A/B evidence above;
   - the drafted [#57](https://github.com/Kb2uka/Omarchy-M4-Native/issues/57)
     comment, asking for bounded GPU checks with a logged value and for the
     shipped module's source;
   - fork PRs #1–#3 to upstream `develop`, after an attribution check against
     [CONTRIBUTING.md](../CONTRIBUTING.md) and a rebase over the overlapping
     upstream [#59](https://github.com/Kb2uka/Omarchy-M4-Native/pull/59);
   - installer follow-ups: run `omarchy-apply-lock`, bind the J713 lid
     switch name, set Brave's Wayland flag and a regulatory domain, enable
     `fstrim.timer`, and consider a glvnd-registered `libEGL` for Xwayland.
6. **Shutdown and the update channel.** alpha.1's missing PMU cells remain;
   Shut Down was not tested. The fix comes through the M4 update channel.
   Whether its boot composer preserves the patched `loader.bin` is
   **unverified**: keep `boot-nodock.bin` and check the ESP hash afterwards.
7. **Hands-on checks still pending:** speakers at low volume, touchpad
   gestures and haptics, F-row and Fn, the keyboard backlight by eye, the
   headphone jack (expected absent), a USB device on the hinge-side port,
   idle battery drain, Bluetooth pairing and audio, and tearing and notch
   placement by eye.

## Lessons for other J713 owners

1. **Check `System Firmware Version` and the macOS build first.** On 26.6.2
   (25G83) the alpha.1 kit refuses at the speaker filters, although the
   25G83 AudioDSP renders the same filters (fork PR #3).
2. **Predict the GPU before partitioning** from the last `perf-states` of
   `ioreg -p IODeviceTree -n sgx`, or with the fork PR #1 preflight. The
   alpha.1 module starts only at exactly 24,800 mW: 1000 mV at 1470 MHz,
   10 cores.
3. **Turn off automatic macOS updates first.** A newer macOS changes the
   AudioDSP, and system firmware cannot be downgraded.
4. **Answer `y` to the "Overhead" warning** when you have no Time Machine
   snapshots; the default `N` quits the installer.
5. **A logo with no text and no keyboard means the hang is below Linux.** If
   the root ext4 superblock, read from macOS, is still 16 GiB and last
   written on the build machine, Linux never ran and powering off is safe.
6. **ESP changes cannot reach stage 1.** If a no-kernel `boot.bin` does not
   bring up "Running proxy…", stage 2 never ran.
7. **Silent stage-1 hangs can be console output.** A non-zero
   `DATA_TX_FREE` at entry does not rule out a full dockchannel FIFO later.
   Try the 8-byte `dockchannel_uart_init` patch in both stages first.
8. **Stub boot changes need the stub's paired 1TR**, which Options opens
   only while Omarchy M4 is the default startup disk; see
   [Recovery gotchas](#recovery-gotchas). With FileVault off, stage files on
   the macOS Data volume (`/Volumes/Data/Users/<macos-user>/…` in Recovery).
9. **The ESP may not mount on macOS.** Raw `dd` reads and guarded writes of
   the partition worked when `diskutil mount` refused it.
10. **The USB proxy needs** a data-capable USB-C cable (stage 2 initialized
    both controllers), a udev `uaccess` rule for `1209:316d` on the host, an
    upstream proxyclient (it matched the release copy at `c9ef56f`), and
    `USE_CLANG=1` plus an `ld.lld` if no aarch64 GCC is installed.
11. **Framebuffer markers survive only outside the console region** (above
    y = 64, or at x ≥ 1536). The stage-1 logo blit erases the block painted
    just before it.
12. **After the desktop works:** run `omarchy-apply-lock`, bind the lid
    switch as `Apple SMC power/lid events`, add `brave-flags.conf`, hide
    Suspend, pin `linux-m4-omarchy` and `aquamarine`, never run
    `omarchy-refresh-pacman`, and back up the unowned M4 stack before the
    first update.

## Evidence and limits

Hardware observations come from screen photos of the marker builds, the
owner's descriptions and the Air's own logs, read over SSH or from raw
partition copies. Source and binary claims come from disassembly and the m1n1
and published G16 sources. Raw photos, logs, images and session records stay
private: they contain machine identities.

- **Inferred, not observed:** that the blocking statement is the
  `pmgr_reset_device()` print; that stage 2 would hang without its patch;
  why the FIFO is not drained on this Air; that boot-args never reached the
  stub; that the GPU budget change is otherwise harmless; that the fixed
  `boot.bin` hash applies to other alpha.1 installs.
- **Unverified:** the clean stage 1 on hardware; the cause of the GPU crash
  and of the no-sleep build's reboot loop; speaker audibility and the other
  hand checks above; suspend and shutdown; 5/6 GHz Wi-Fi; Bluetooth
  pairing; any other Air unit.
