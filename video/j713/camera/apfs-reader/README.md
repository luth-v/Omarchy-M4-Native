# Build and test the camera APFS reader

See [source notices and limits](SOURCE-NOTICES.md) for the pins, licenses,
protocol and refusal codes. The installed helper accepts an inherited read-only
block descriptor. Only developer test builds enable image input.

The helper follows the libevdi and GStreamer package-input pattern. Build it
out of band, reproduce its bytes in a second clean output directory, pin its
SHA256 in the camera recipe and channel policy, and add a `rebuilt-identical`
source join. The maintainer uploads that exact binary and mirrors both upstream
archives to `package-inputs`. Channel CI on `ubuntu-24.04` only packages those
bytes. It does not compile the reader. Its official runtime dependencies are
`glibc`, `gcc-libs`, `zlib`, `bzip2` and `util-linux` for `setpriv`.

## Pinned build

The current input is `m4-apfs-reader-cc9e96b8`, SHA256
`cc9e96b8854005154e0f49d6aaa630dd9276d4d99ab9b23777b385dd9551e225`.
It was cross-built on x86_64 with Clang/LLVM `23.1.3-4`, CMake `4.4.4-1`,
GNU make `4.4.1-3`, LLD `23.1.3-4` and the ALARM GCC 16.1.1 sysroot.
Three separate clean builds reproduced it, including one with the checked-in
[cross toolchain](cross-toolchain.cmake). [cross-inputs.json](cross-inputs.json)
records every sysroot package and LLD by exact version, filename and SHA256.
[host-inputs.json](host-inputs.json) pins the x86_64 Clang, llvm-libs, CMake and
make archives. The host packages and LLD use the
[Arch Linux Archive](https://archive.archlinux.org/).

ALARM has no package archive. Its URLs are rolling-mirror locations that may
stop serving these versions. Reproduction needs the exact eight ALARM files
named in `cross-inputs.json`, with those SHA256 values. Preserve copies of
them; a different mirror is usable only if it supplies the same pinned bytes.
This cross-build recipe is part of the camera reader contribution.

On a MacBook Air M4 with macOS 26.6.2 / 25G83, package `1-2`, SHA256
`3d22d60bb9d86b39e0faa0455bd950a2f7296da2aa4880854aab4c8d2266f3d5`,
recovered the removed record before reboot. The installed helper matched the
pin above, and the recovered record was root:root 0644, 28,660 bytes, SHA256
`1e25d89b7acc3b62fcb64c5102503ba289c3cb000ca6abe812c3249ac6349f77`.
Inspection as `nobody`, with no capabilities and `no_new_privs`, completed
without refusal reasons on internal APFS partitions p1, p2, p3 and p7.
The [camera guide](../../../../docs/j713-camera.md) records their volumes.

Obtain the exact apfs-fuse and lzfse archives named in the notices. The
maintainer mirrors them as
[apfs-fuse archive](https://github.com/Kb2uka/Omarchy-M4-Native/releases/download/package-inputs/apfs-fuse-66b86bd525e8cb90f9012543be89b1f092b75cf3.tar.gz)
and [lzfse archive](https://github.com/Kb2uka/Omarchy-M4-Native/releases/download/package-inputs/lzfse-e634ca58b4821d9f3d560cdc6df5dec02ffc93fd.tar.gz).
These uploads are required before CI can fetch them. `build.sh` checks both
archive hashes before extracting or building. With those archives in `SOURCES`,
prepare an x86_64 build environment with the exact host packages from
`host-inputs.json` installed. The loop below verifies their archives as well
as the cross inputs, then extracts only the sysroot and LLD. Prepare `CROSS`
from those pinned files:

```bash
# SOURCES and CROSS are absolute paths; CROSS is a new scratch directory.
mkdir -p "$CROSS/sysroot" "$CROSS/tools"
python3 - "$CROSS" <<'PY'
import hashlib, json, pathlib, subprocess, sys, urllib.request
root = pathlib.Path(sys.argv[1])
recipe = pathlib.Path('video/j713/camera/apfs-reader')
host = json.loads((recipe / 'host-inputs.json').read_text())
cross = json.loads((recipe / 'cross-inputs.json').read_text())
for item in host + cross:
    archive = root / item['filename']
    if not archive.exists():
        with urllib.request.urlopen(item['url'], timeout=60) as response:
            archive.write_bytes(response.read())
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == item['sha256'], archive
    if item in host:
        continue  # These exact packages must already be installed in the build environment.
    target = root / ('tools' if item['name'] == 'lld' else 'sysroot')
    subprocess.run(['tar', '-xf', str(archive), '--no-same-owner', '-C', str(target)], check=True)
PY
export M4_APFS_SYSROOT="$CROSS/sysroot"
export M4_APFS_LLD="$CROSS/tools/usr/bin/ld.lld"
export LD_LIBRARY_PATH="$CROSS/tools/usr/lib"
# Clang and llvm-libs must both be 23.1.3-4. No network is used by build.sh.
bash video/j713/camera/apfs-reader/build.sh "$SOURCES" "$CROSS/build-a" \
  video/j713/camera/apfs-reader/cross-toolchain.cmake
bash video/j713/camera/apfs-reader/build.sh "$SOURCES" "$CROSS/build-b" \
  video/j713/camera/apfs-reader/cross-toolchain.cmake
cmp "$CROSS/build-a/m4-apfs-reader" "$CROSS/build-b/m4-apfs-reader"
sha256sum "$CROSS/build-a/m4-apfs-reader"
```

The build fixes `SOURCE_DATE_EPOCH=0`, maps source paths, builds only static
libraries and the helper, disables fixture mode and sanitizers, and removes
unused sections. It saves the input hashes, ELF details and CMake toolchain
logs in each output directory. Changes to source or toolchain require a new
binary pin and hardware re-test. The package's `!strip` option preserves it.
Never rebuild the helper during the channel package job or substitute a new
build for the binary the contributor tested.

For a native aarch64 developer build, the same script accepts just `SOURCES`
and a new output directory. It needs gcc/g++, CMake, make, patch, binutils,
zlib and bzip2 development files. Such a build does not replace the current
pin automatically. Its bytes must be pinned and tested before publication.

## Offline tests

The repository smoke test verifies the two archives, builds production and
fixture variants and inspects a wholly synthetic empty container. It also
checks invalid NXSB and descriptor refusals. CI fetches the mirrored sources:

```bash
M4_APFS_SOURCES="$SOURCES" \
  python3 -m unittest discover -s tests -p test_m4_apfs_reader.py -v
```

For sanitizer fixtures, use a source tree patched by `build.sh`, set `APFS` to
that absolute directory, and set `FIXTURES` to the external regular-file fixture
directory containing `dfvfs-apfs.raw` and `apfs-content-padded.apfs`:

```bash
cmake -S video/j713/camera/apfs-reader -B .artifacts/reader/sanitized \
  -DAPFS_SOURCE_DIR="$APFS" -DM4_APFS_TEST_IMAGES=ON -DM4_APFS_SANITIZERS=ON
cmake --build .artifacts/reader/sanitized --target m4-apfs-reader fixture-layout -j4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 video/j713/camera/apfs-reader/tests/offline.py \
  --helper .artifacts/reader/sanitized/m4-apfs-reader \
  --layout .artifacts/reader/sanitized/fixture-layout --fixtures "$FIXTURES" \
  --scratch .artifacts/reader/generated
```

The fixture script uses only regular files. It tests public plain/LZVN hashes,
symlinks, skipped keybags, encryption, incompatible features, corrupt nodes,
short images, overlong decompression, snapshots and sealed extents. Synthetic
sealed images are not Apple-sealed snapshots. These regular-file tests cover
the parser separately from the pre-reboot Air package evidence above.
