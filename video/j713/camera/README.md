# J713 camera sources

The production V4L2 driver exposes default NV12 1280x720 and optional
1920x1080 at 30 fps. It defaults to `colour=1`, `framing=2` and `fps=1`,
delivers each acknowledged frame immediately, and restarts the firmware for
each session. See [J713 camera](../../../docs/j713-camera.md) for operation
and limits.

1080p appears only when the probe-time claim obtained the extra 5.75 MiB
tail. At each original start, try the 1080p size and then the original
size at the same start. The 720p placement, refusal and firmware requests
stay unchanged. 1080p keeps preset 0, requests 1920x1888 with `0xb01`, sizes
image pool 1 with `0x117`, and host-crops Y/UV at rows 404/202. It requires
fresh `[MSC] CH = 0x0 BES[0] originalOutputSize [1920 1888]` evidence through
the first capture list. The V4L2 intervals and stream parameters offer 30 fps
only, and session pins clamp 60 to 30; 1080p60 is unmeasured.

`kernel/` contains the driver and `j713_isp_d_dt` overlay loader. The Stage D
node and exported symbol names remain for compatibility with the validated
lifecycle. Research controls and firmware memory peeks are removed. The
loader accepts only the qualified code CRC in `j713_cam_qualified.h` before
powering the ISP or applying the overlay. The camera checks it again at
probe and before each firmware start.

`tools/build-j713-camera.sh` builds both modules against the channel's prepared
kernel tree, with the same input hashes as the headphone driver. Pass the
media-only exports from the release's matching `kernel-decoder/Module.symvers`,
the immutable local builder image ID, and a new output directory:

```bash
bash tools/build-j713-camera.sh KERNEL_BUILD MEDIA_EXPORTS IMAGE_ID OUTPUT
```

Publish the media-only table alongside the module inputs as
`source/reference-media-exports.symvers`, as the headphone build publishes its
sound exports. The table contains kernel symbol metadata, not Apple data.

The media exports are the `drivers/media/` lines of that `Module.symvers`, and
have SHA256
`ae0764effc35075fedb4e3492d973a655e963e612cba1e28a1609b6d4f876285`:

```bash
awk '$3 ~ /^drivers\/media\//' kernel-decoder/Module.symvers > reference-media-exports.symvers
```

The release kernel and prepared tree share configuration SHA256
`14b4437b2b195e912a2ab614d28761acd3dc79f886d4487964aa0912616a3a6e`.
The build uses Clang 21.1.8, `W=1 KCFLAGS=-Werror`, read-only kernel inputs,
`--network none --pull never`, and checks AArch64 and vermagic
`7.1.9-m4-gpu-backport-20261008`. It neither loads nor installs a module.
Compare the resulting modules with the package/channel pins. No binary belongs
in this source directory.

Run `bash video/j713/camera/kernel/tests/run.sh` for eleven C programs under
ASan/UBSan and three replay checks. Tests execute extracted production
functions against synthetic inputs. The production fixture was also replayed
from the reviewed source corresponding to module
`82f9fd253d731857c162ef6fc61bd8907e638687bf45da6379270dbc19e95a33`.
The original eight sessions, now checked with and without the 1080p tail,
cover both firmware-output geometries, colour 0/1 and first/later firmware
sessions, with framing 2 and FPS follow mode. Complete
requests, capture pool, IPC and historical logs match 67,697,262 bytes,
SHA256 `f8fded6d36d8daf3aa091f11c5051d44f737f487b287a2b4091251f6aeda3981`.
Only the host pacing summary, absent from that historical fixture, is omitted
by its retained writer. The separate pacing tests check its counters and the
per-frame delivery path.

An additional eight-session 1080p replay was recorded from the unchanged
source corresponding to module
`85fe34f2748d790325692daee6129be219b9a204b841b6db556d1278582b33ff`.
It covers colour 0/1, framing 2, FPS follow mode with requested 30/60 fps,
and first/later firmware sessions. Complete requests, capture pool, IPC
and historical logs match 115,922,986 bytes, SHA256
`2a27f322b8c0fe96cf969e08dc926b751b70cc6fd25575ca967ef48b649478d3`.
The gate, claim, crop, white balance, format and 30 fps regressions run
against production functions. Run the review mutations in disposable sources:

```bash
python3 -I video/j713/camera/kernel/tests/mutate_highres.py SCRATCH_DIRECTORY
```

All 61 applicable 1080p mutations and two 720p gate/watcher regressions
are rejected. The four runner mutations are omitted because the runners
are not shipped here. `run.sh` does not run the mutations; they rebuild the
tests for each one.

A hardware test on 2026-10-19 validated the out-of-tree `85fe34f2…` module with
150/150 1080p frames, zero drops, copy average 4569us and pacing average
33112us. 720p before and after was normal. 1080p was not tried in a browser
or under v4l2-compliance.

On 2026-10-21 the contributor's MacBook Air M4 also ran the packaged
`ddcdf324…` and `c6d115ec…` modules, loaded by package `1-1`'s boot service.
Both 720p and 1080p captures delivered 150/150 frames with zero drops;
1080p accepted firmware output 1920x1888, and a later 720p capture was normal.
After a subsequent power cycle with final package `1-3` (`ce22df3b…`), the
same modules ran three 720p Meet sessions. These runs used the original
`20261008` kernel build and contributor's own boot chain. Camera loading on
the retained-bounds rebuilt image, channel devicetree and new display handoff
stage 2 remains untested on hardware. See the
[hardware evidence and limits](../../../docs/j713-camera.md#1080p-at-30-fps).

Apple firmware, macOS binaries, tuning files, captures and research logs are
not part of these sources. The tuning file comes from the owner's macOS
installation at install/update time.
