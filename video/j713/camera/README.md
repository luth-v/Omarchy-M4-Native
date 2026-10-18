# J713 camera sources

The production V4L2 driver exposes NV12 1280x720. It defaults to `colour=1`,
`framing=2` and `fps=1`, delivers each acknowledged frame immediately, and
restarts the firmware for each session. See [J713 camera](../../../docs/j713-camera.md)
for operation and limits.

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

Run `bash video/j713/camera/kernel/tests/run.sh` for nine C programs under
ASan/UBSan and the retained production replay. Tests execute extracted
production functions against synthetic inputs. The production fixture was
also replayed from the reviewed source corresponding to module
`82f9fd253d731857c162ef6fc61bd8907e638687bf45da6379270dbc19e95a33`.
Its eight sessions cover both firmware-output geometries, colour 0/1 and
first/later firmware sessions, with framing 2 and FPS follow mode. Complete
requests, capture pool, IPC and historical logs match 67,697,262 bytes,
SHA256 `f8fded6d36d8daf3aa091f11c5051d44f737f487b287a2b4091251f6aeda3981`.
Only the host pacing summary, absent from that historical fixture, is omitted
by its retained writer. The separate pacing tests check its counters and the
per-frame delivery path.

Apple firmware, macOS binaries, tuning files, captures and research logs are
not part of these sources. The tuning file comes from the owner's macOS
installation at install/update time.
