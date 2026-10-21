# APFS camera reader sources and notices

The separately invoked `m4-apfs-reader` is GPL-2.0-or-later. The Python
owner-data tool remains MIT and communicates with it through a subprocess.
The package contains no Apple proprietary data. The helper has no FUSE dependency.

The out-of-band build uses these pinned, checksummed upstream source archives,
mirrored by the maintainer in the `package-inputs` release:

- [apfs-fuse](https://github.com/sgan81/apfs-fuse/tree/66b86bd525e8cb90f9012543be89b1f092b75cf3),
  revision `66b86bd525e8cb90f9012543be89b1f092b75cf3`, GPL-2.0-or-later.
  Archive SHA256 `e78e71e87d8ba182822ec34233189141b8816c9a3f700c9a22f6dc1480afee51`.
  Its copyright headers and `LICENSE` remain in the corresponding source.
  `apfslib-readonly.patch` contains this project's changes to that source.
- [lzfse](https://github.com/lzfse/lzfse/tree/e634ca58b4821d9f3d560cdc6df5dec02ffc93fd),
  revision `e634ca58b4821d9f3d560cdc6df5dec02ffc93fd`, BSD-3-Clause.
  Archive SHA256 `ca98aa6644d44500e3315858daa747ce15bd06d49e3edb12a5458e5525e8ebdb`.
  Its `LICENSE` and source notices remain in the corresponding source.

Both upstream license texts ship in `usr/share/licenses/omarchy-m4-camera/`
and are retained verbatim here. The [build recipe](README.md), `build.sh`,
helper, CMake files, read-only patch and these exact archives are the
corresponding build source. The helper is built out of band and uploaded as
`m4-apfs-reader-cc9e96b8`; the PKGBUILD packages that pinned binary unchanged.
`packaging/channel-source-joins.json` joins it to the source commit and mirrored
archives. The two camera module joins retain their module hashes, and the
module/health `SHA256SUMS` stays unchanged.

## Changes to ApfsLib

The patch bounds geometry, checkpoint scans, metadata operations, B-tree depth,
key/value offsets, inode fields, xattrs, file extents and decompression. It checks
short reads and decoded lengths, propagates read failures, fixes unaligned
scalar loads and exception ownership, and removes password/decryption branches.
It deliberately never initializes or reads a container keybag. Unknown
incompatible System features and encrypted metadata/streams are refused.
Compression methods 3/4, 7/8, 9/10 and 11/12 are enabled. The experimental
LZBitmap methods 13/14 are refused.

Python opens only validated internal SSD APFS partitions read-only, checks the
block inode against sysfs and requires an Apple ANS (`nvme-apple`) controller
ancestor. Python duplicates the descriptor to a number of at least 3 and passes
it to every helper call. `/usr/bin/setpriv` runs the parser as `nobody`, with
no extra groups, no capabilities in any set, and `no_new_privs`. The already-open
read-only descriptor stays readable after the privilege drop.
The helper duplicates it with `F_DUPFD_CLOEXEC`, requires read-only block input,
checks NXSB/checksums and uses `pread`. This avoids reopening a path between
validation and parsing and places device reads inside the timed child. It
never calls the generic Device/DMG opener. Production cat accepts only
`/usr/sbin/appleh16camerad` in an unencrypted System volume. Snapshot selection
changes the library's in-memory view; it performs no kernel mount.

The helper limits files/xattrs/compressed inputs to 128 MiB, snapshot results to
64 per volume, snapshot scans to 256 entries, volume slots to 100, checkpoint
descriptors to 32,768, B-tree depth to 64, metadata/device operations to one
million and device reads to 4 GiB. It has a 1 GiB address-space cap and a
175-second alarm. Python adds 120 seconds per child, a 240-second overall
budget, a 1 MiB metadata cap and a separate 64 KiB diagnostic cap. File bytes
stay in bounded pipes. Per-partition and per-volume refusals contribute no
record from that source. The tool retains at most 64 named source diagnostics
and 64 reason counts, and prints them only if no record qualifies. Success
reports the selected partition, volume and view and the skipped-source count.
An inspect `refusal` row excludes that volume, including any partial snapshot
list.
Incomplete snapshots and failed path lookups skip that view; a lookup failure
is named `PATH_LOOKUP_ABSENT_OR_INVALID` because ApfsLib does not distinguish
absence from corrupt metadata there. Timeouts, the overall budget, output caps
and the helper's deadline remain hard aborts. The kill/reap wait is bounded
inside the call deadline, without a subprocess context manager's unbounded wait.

Different records inside one daemon abort extraction. A view with an
unqualified record is ignored. Identical qualified records deduplicate; two
different qualified records would abort, but the current single hash pin
admits only one identity. Only the pinned record can be installed. If no
source qualifies, the tool summarizes the collected reasons and leaves the
camera unavailable; the update continues.

Exit codes are 0 for completed inspection or file output, 21 for a named
container/file refusal, 22 for allocation failure and 24 for the helper deadline.
Inspection reports volume-local refusals as JSON rows and continues other
volumes. Caught parser signals exit nonzero with `REFUSAL PARSER_SIGNAL`.
Encrypted and non-System volumes are inspected only at the superblock and
skipped without opening their filesystem trees. No credentials, decryption,
input writes or mount interface is exposed. Unused generic Device/DMG and
password code is removed at link time with function/data sections and garbage
collection.

`M4_APFS_TEST_IMAGES=ON` enables regular-file fixtures and arbitrary fixture
paths. It defaults to OFF; the out-of-band input build explicitly keeps it OFF.
`M4_APFS_SANITIZERS=ON` omits the address-space cap and custom fault handlers
for ASan diagnostics only; the out-of-band input build explicitly disables it.

## Offline fixtures

The build smoke test creates a wholly synthetic empty APFS container and tests
its inspection, invalid NXSB and descriptor refusals. No third-party image is
committed. `tests/offline.py` optionally exercises external dfVFS and
apfs-forensic regular-file fixtures, with generated snapshot/OMAP/fext and
refusal cases kept in a caller-specified scratch directory. Their image
redistribution rights have not been established here, so those images and their
modified derivatives remain outside the tree and package. This source code
contains no data from them. Public LZVN file hashes are useful compression
oracles; synthetic sealed/snapshot images are parser tests, not an Apple seal.
