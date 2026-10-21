# J713 built-in camera

## Symptom

The MacBook Air M4 J713 has no built-in camera device in Linux. Its H16 ISP
needs board-specific DART mappings, power sequencing, RTKit transport and
sensor tuning before V4L2 applications can use it.

## What works

The owner's reviewed driver exposes `FaceTime HD Camera (J713)` as a V4L2
capture device, with single-plane NV12 at default 1280x720 and optional
1920x1080 at 30 fps. It delivers each rendered frame after its return
acknowledgement, without holding the first frame for its paired buffer.
PipeWire and browser use at 720p were validated on one J713 with ISP code
CRC `aaafbaa6` and kernel `7.1.9-m4-gpu-backport-20261008`. 1080p was
validated on the same Air in a 150-frame capture and a live preview only.

This contribution ports that source into
[`video/j713/camera/`](../video/j713/camera/README.md). On the contributor's
MacBook Air M4, the packaged modules ran 720p and 1080p captures on
2026-10-21, as detailed below. Package `1-3` (`ce22df3b…`) later recovered
owner tuning and, after a physical power cycle, its boot service loaded the
same modules. Google Meet completed three 720p sessions on that boot, with
pacing averages of 32.3 to 33.3 ms and a 51.7 ms maximum.

These hardware runs used the contributor's own boot chain and original
`20261008` kernel build. They did not use the retained-bounds rebuilt image
from [#157](https://github.com/Kb2uka/Omarchy-M4-Native/pull/157), the channel
devicetree or the new display handoff stage 2. Static checks confirm the
rebuilt image has the same release and export pin; these camera modules have
never loaded on that image on hardware. Fresh installation and the first
update from an older setup script remain unverified.

## Defaults

A boot load passes no module parameters. The built-in defaults are:

- `colour=1`: load the canonical sensor tuning, with the lens-shading grid
  disabled in the capture-pool copy. The owner's file stays unchanged.
- `framing=2`: pin zoom to 1x, check it after the first requeue, and reassert
  it on drift at the existing 60-delivered-frame cadence.
- `fps=1`: follow the application's V4L2 interval, 30 fps unless it requests
  60 at 720p. At 1080p it offers, reports and pins 30 fps only. Only 30 fps
  is qualified here; advertising 60 at 720p does not qualify it.
- `out_height=1264`: the firmware produces 1280x1264 and the driver crops
  the middle 720 rows for the default V4L2 size. `out_height=720` retains
  the earlier 720p geometry with its aspect error; it adds no format.
  Neither setting changes the 1920x1888 firmware output used for 1080p.

`colour` accepts 0/1, `framing` accepts 0/1/2 and `fps` accepts 0/1/30/60.
They latch at session start; writes during a stream affect the next session.
Mode 0 disables that control. Framing 1 reports crop and zoom at 720p, or
zoom at 1080p, without pinning. The validated restart timing controls retain
their zero defaults. Firmware restart is always on. Research peeks, firmware
alias writes, raw setfile and lens-calibration controls, full-span fallback
and debug toggles are removed.

## 1080p at 30 fps

Select 1920x1080 NV12 in the application before allocating capture buffers.
1280x720 stays the default. The selected format persists across opens; an
application that does not set a format inherits the previous selection.
1080p offers only 1/30, including when `fps=60` or the application requests
1/60. A requested 60 logs `fps: 60 not offered at 1080p, using 30` once per
session. The application's 720p rate choice is kept when switching back.
1080p60 has not been measured.

1080p appears only when the probe-time boot claim got the extra 5.75 MiB
tail. At each original claim address the driver tries the larger size,
then the original size at the same start before moving on. A busy tail
therefore leaves the original 720p placement and refusal behavior intact.
The probe line ends in `NV12 1280x720, 1920x1080` when the tail was free,
or `NV12 1280x720; no 1080p tail` otherwise. Without it, the frame-size
list contains only 720p and 1080p requests normalize to 720p. No capture-time
claim or mapping is needed. With the tail, the identity mapping extends
5.75 MiB beyond the original 720p pool; 720p requests never name that range.

The 1080p route keeps sensor preset 0 and asks for 1920x1888 firmware
output. The driver crops Y/UV at rows 404/202 into 1920x1080. Fresh
`[MSC] CH = 0x0 BES[0] originalOutputSize [1920 1888]` evidence is required
for that session, checked through the first capture-list exchange. Missing,
mismatched, replaced or lost evidence before acceptance fails closed with
an ordered stop; the next attempt starts fresh firmware. A replacement
during capture errors the buffer and queue. Success logs
`1080p: firmware output accepted 1920x1888`.

A hardware test on 2026-10-19 validated the out-of-tree module `85fe34f2…`, not
these packaged modules. Probe offered both sizes; 1080p `capture 150`
accepted 1920x1888 and delivered 150/150 frames with zero drops,
`copy_avg 4569us` and pacing average 33112us. 720p behaved normally before
and after. A 10-second recording lost frames while the runner's
live libx264 encoding ran at 0.587x; the driver produced 28.7 fps. This
branch does not ship that runner. Offline replay links the packaged code
to the validated source. These measurements used the out-of-tree module.

On 2026-10-21, the contributor's MacBook Air M4 boot `dee383b8` had
`omarchy-m4-camera` 1-1 installed. `m4-j713-camera.service` loaded
`j713_isp_cam.ko` `ddcdf324…` and `j713_isp_d_dt.ko` `c6d115ec…`,
byte-identical to the final packaged modules. The probe offered
`1280x720, 1920x1080`. The 720p capture delivered 150/150 frames with zero
drops, copy average 2177 µs and pacing average 33113 µs. The 1080p capture
accepted firmware output 1920x1888 and delivered 150/150 frames with zero
drops, copy average 4894 µs and pacing average 33113 µs. A subsequent 720p
capture was normal. These captures do not qualify longer 1080p sessions or
1080p in browsers and PipeWire.

## Delivery and boot

`omarchy-m4-camera` 1-3 arrives through `omarchy-m4-update`, with
`omarchy-m4-boot` 1-20 depending on it so an older installed setup script also
gets it. Run the update, shut down fully, then power on:

```bash
omarchy-m4-update
```

The package stages two pinned modules, the tools, a health check and a checksum
manifest under `/usr/lib/omarchy-m4/j713-camera/`, plus `/usr/bin/m4-camera`,
a modprobe fragment preventing automatic alias loads, and
`m4-j713-camera.service`. Installation enables the service and prepares owner
tuning. It does not load the modules or start the firmware.

The service runs after udev and speaker registration. It has no dependency
that makes the speaker service or login fail if the camera refuses. Its
`Type=exec` start does not make the boot targets wait for module loading.
The tool checks the board, kernel, module pins, file ownership and permissions,
owner tuning, host health, shared lock and previous attempts before loading
`j713_isp_d_dt`, the V4L2 dependencies and `j713_isp_cam`. The loader checks
firmware identity before powering ISP sub-domains or applying its overlay.
The firmware starts at the first STREAMON.

One load attempt is allowed per boot. The tool writes and syncs
`/var/lib/omarchy-m4/camera/attempt` before loading any module. A crash or
interrupted load leaves that strike, so the next boot loads nothing.
`sudo m4-camera --start` explicitly clears a stale strike and tries in the
running session, under the same lock and checks. It cannot reuse a spent
load attempt or recover a DEAD driver in that boot. Physical power-off is
required in those cases. Nothing unloads or automatically retries.

## Owner tuning and firmware qualification

The tuning source is the owner's macOS `/usr/sbin/appleh16camerad`. The
qualified record was extracted from macOS 26.6.2, build 25G83. It appears
in both slices of that universal binary. The extractor scans 4 KiB-aligned
32-byte big-endian headers for `0x22240106`, validates the record's sensor
`0x958`, and extracts exactly `0x6ff4` bytes. Identical duplicate records
in one daemon are allowed; different records inside that daemon abort extraction. Output must have SHA256
`1e25d89b7acc3b62fcb64c5102503ba289c3cb000ca6abe812c3249ac6349f77`.

The macOS installer reads that installed daemon directly and adds only the
qualified record to the existing owner firmware package. On Linux,
installation, package upgrades and subsequent `omarchy-m4-update` runs use
`m4-apfs-reader`, a separate GPL-2.0-or-later executable under
`/usr/lib/omarchy-m4/j713-camera/`. The maintainer uploads the hash-pinned helper built out of band from apfs-fuse
and lzfse sources with the read-only patch; channel CI only packages it. It has no FUSE
dependency, and the `7zip` dependency is gone. See the
[source and notices](../video/j713/camera/apfs-reader/SOURCE-NOTICES.md).

Python selects only APFS partitions of the non-removable internal SSD
`/dev/nvme0n1`, checks their sysfs block identity and Apple ANS controller
ancestry (`nvme-apple`), and passes an already-open read-only descriptor
numbered at least 3. `setpriv` runs the helper as `nobody`, without extra groups
or capabilities, with `no_new_privs`. The descriptor stays readable after
the privilege drop. The helper validates NXSB and
checksums inside the timed child, skips keybags entirely and opens no encrypted
or non-System filesystem tree. It inspects unencrypted System volumes, tries
up to 64 enumerated snapshots per volume and then the live tree. It reads only
`/usr/sbin/appleh16camerad`. Each partition, volume or view refusal excludes
only that source and logs a bounded named reason. Different records inside one
daemon abort extraction; an unqualified record in another view is ignored.
Identical qualified records deduplicate across views. Two different qualified
records would abort, but the current single hash pin admits only one identity. It does not claim that
an enumerated snapshot is the currently booted macOS snapshot.

No partition is mounted or written, no daemon is executed and no data is
downloaded by the installed tools. The installed binary has no regular-image
mode. File/xattr/compressed inputs have a 128 MiB cap, metadata and diagnostic
outputs have separate caps, and the parser bounds checkpoint scans, B-tree
traversals and decompression. Python imposes 120 seconds per helper call and
one four-minute budget for the entire sweep. Daemon bytes stay in bounded
memory pipes. Refused volumes, incomplete snapshots and failed path lookups
can fall through to other sources. `PATH_LOOKUP_ABSENT_OR_INVALID` covers both
an absent name and failed metadata lookup. Timeouts, exhausted total budget
and output caps abort installation even after an earlier source qualifies.
The kill/reap wait also has a deadline. If nothing qualifies, collected reasons
are printed with at most 64 source diagnostics and 64 reason counts. Success
prints one line with the selected partition, volume, view and skipped-source
count. A qualified existing file is reused without reading a partition.
A different existing file is preserved and reported, never silently replaced.

The destination is `/usr/lib/firmware/apple/isp_2224_01XX.dat`, root-owned
at 0644. No Apple proprietary bytes belong to the channel package or this repository.
The boot tool verifies its size and hash before loading. Missing, encrypted,
unreadable or unqualified macOS data leaves the camera unavailable and
reports the reason; updates and the rest of the system remain usable.
The package hook and update setup can each run the owner tool during an
install or upgrade. A successful hook makes the setup check reuse the qualified
file immediately. If it fails, setup can try again, so two failed sweeps can
take up to eight minutes in that update. Later updates normally reuse the file.

Until qualified tuning is installed, `omarchy-m4-update` does not ask for a
restart on the camera's behalf, because the boot would refuse to load it.
On a MacBook Air M4 with macOS 26.6.2 / 25G83, package `1-2`, SHA256
`3d22d60bb9d86b39e0faa0455bd950a2f7296da2aa4880854aab4c8d2266f3d5`,
recovered the record after its removal with a verified backup, before reboot.
The hook scanned internal APFS partitions p1, p2, p3 and p7. Non-System and
encrypted volumes were skipped. The p3 unsealed System volume returned
`PATH_LOOKUP_ABSENT_OR_INVALID`. No skip was fatal, and the hook reported
qualified owner tuning ready. The recovered record matched the SHA256 above,
was root:root 0644 and 28,660 bytes. The installed helper matched SHA256
`cc9e96b8854005154e0f49d6aaa630dd9276d4d99ab9b23777b385dd9551e225`,
and the service was enabled.

The installed helper also completed inspection as `nobody` (uid 65534),
with no capabilities and `no_new_privs`, with exit 0 and no refusal reasons
on all four partitions. Their volumes were:

- p1: iSCPreboot, xART, Hardware and Recovery.
- p2: Macintosh HD (System, sealed, one snapshot), Preboot, Recovery, Update,
  Data (encrypted) and VM.
- p3: Omarchy M4 - Data (encrypted), Omarchy M4 (System, unsealed), Preboot
  and Recovery.
- p7: Recovery (one snapshot) and Update.

An earlier read-only reader test on the same macOS build returned identical
9,130,288-byte daemons from the p2 live tree and snapshot `423209`, SHA256
`1b55f1f4…6cef`, and the qualified record. The container checkpoint was
`459604`, block size 4096, and the sealed System volume had incompatibility
flags 33. These observations contain only metadata and hashes.

Firmware qualification is separate. The loader's list in
`j713_cam_qualified.h` contains only ISP code CRC32 `aaafbaa6`, for the
`0xca4000`-byte code segment and fixed J713 physical layout from the
owner's macOS 26.5.1 / 25F80 boot firmware. Unknown code returns `-ENODEV`
before power writes or overlay application and logs
`unqualified ISP firmware CRC ...; camera stays unloaded`. The camera
also requires the qualified CRC at probe and each firmware start; the boot
tool checks the loader's reported CRC before camera loading. A macOS or
firmware update requires a new qualified identity and hardware evidence.
This CRC gate is a compatibility check, not a cryptographic signature.

## Switch it off

```bash
sudo m4-camera --disable
```

This writes `/var/lib/omarchy-m4/camera/hold`. Nothing loads on subsequent
boots until `sudo m4-camera --enable` removes it. Power off fully after
disabling to release an already loaded camera. The package remains installed
and receives updates. Enabling does not start a load; power on again to
load at boot. The hold survives package updates and removal.

## Troubleshooting

`m4-camera` inspects without loading or requiring root. Check its `status`,
`tuning`, `files_verified`, kernel/vermagic, `driver`, `firmware_crc`, hold
and strike fields. The camera node is found by name; `video0` may already
belong to the Apple decoder.

```bash
m4-camera
journalctl -b -u m4-j713-camera.service
journalctl -k -b
```

For missing tuning, inspect the owner-data message from `omarchy-m4-update`.
An unavailable APFS source or different tuning hash needs investigation and
qualification, not an arbitrary downloaded blob. For a kernel mismatch,
update through the channel once matching modules are qualified. A DEAD
reason or spent attempt needs physical power-off before another camera
attempt. A strike after an interrupted load needs an explicit `--start`
in a later healthy running session. Do not clear it during an active load.

## Limits

- 1080p NV12 offers 30 fps only. 720p remains the default; its advertised
  60 fps is unqualified.
- The lens-shading grid is not applied. Corners remain darker.
- Suspend, freeze and hibernation are refused after the first firmware
  start, even after a healthy close has powered the ISP down.
- Every streaming session restarts the firmware. Opening has that startup cost.
- The LED takes about one second to go off after closing.
- The module cannot be unloaded after first use. Its firmware mappings and
  roughly 599 MiB of host memory, plus the optional 5.75 MiB 1080p tail,
  remain held until physical power-off.
- Memory fragmentation can prevent the initial claim. There is no retry in
  the same boot. A busy 1080p tail alone leaves 720p available.
- Qualification covers the contributor's J713 and firmware CRC `aaafbaa6`.
  Other models, firmware builds, the channel boot chain and long-term
  stability remain unqualified.

The offline tests retain the production replay, strict parameter validation,
allocator and restart lifecycle checks, signal-safe worker execution,
frame-by-frame delivery, FPS fallback ordering and V4L2 interval checks.
The 1080p tests cover the gate, crop and white balance, claim fallback,
format negotiation and 30 fps clamp. 720p replay stays byte-identical to
the prior port and `82f9fd25…`, with and without the extra tail; 1080p
replay matches unchanged `85fe34f2…` source.
See the [source/build record](../video/j713/camera/README.md). Fresh
installation, first-update delivery from an older setup script, boot/strike
recovery and interaction with the protected speaker stack need physical
acceptance on the channel boot chain. So do 1080p in PipeWire and browsers,
v4l2-compliance with both sizes listed, longer 1080p sessions and a probe
without the 1080p tail.
