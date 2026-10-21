#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build a package input out of band, never during the channel package job.
# Usage: build.sh SOURCES NEW_OUTPUT [CMAKE_TOOLCHAIN]
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
sources=$(realpath -- "$1")
out=$(realpath -m -- "$2")
apfs=apfs-fuse-66b86bd525e8cb90f9012543be89b1f092b75cf3
lzfse=lzfse-e634ca58b4821d9f3d560cdc6df5dec02ffc93fd
test ! -e "$out"
(
  cd "$sources"
  sha256sum --check --strict <<'SUMS'
e78e71e87d8ba182822ec34233189141b8816c9a3f700c9a22f6dc1480afee51  apfs-fuse-66b86bd525e8cb90f9012543be89b1f092b75cf3.tar.gz
ca98aa6644d44500e3315858daa747ce15bd06d49e3edb12a5458e5525e8ebdb  lzfse-e634ca58b4821d9f3d560cdc6df5dec02ffc93fd.tar.gz
SUMS
)
mkdir -p "$out/source"
tar xzf "$sources/$apfs.tar.gz" -C "$out/source"
tar xzf "$sources/$lzfse.tar.gz" -C "$out/source"
cp -a "$out/source/$lzfse/." "$out/source/$apfs/3rdparty/lzfse/"
patch --batch --fuzz=0 -p1 -d "$out/source/$apfs" < "$here/apfslib-readonly.patch"
toolchain=()
if [[ -n ${3:-} ]]; then
  toolchain=("-DCMAKE_TOOLCHAIN_FILE=$(realpath -- "$3")")
fi
export SOURCE_DATE_EPOCH=0
cmake -S "$here" -B "$out/build" -DAPFS_SOURCE_DIR="$out/source/$apfs" \
  -DCMAKE_BUILD_TYPE=Release -DM4_APFS_TEST_IMAGES=OFF -DM4_APFS_SANITIZERS=OFF \
  "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -ffile-prefix-map=$out=. -ffile-prefix-map=$here=reader" \
  "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -ffile-prefix-map=$out=. -ffile-prefix-map=$here=reader" \
  "${toolchain[@]}"
cmake --build "$out/build" --target m4-apfs-reader -j4
cp "$out/build/m4-apfs-reader" "$out/m4-apfs-reader"
LC_ALL=C readelf -h "$out/m4-apfs-reader" | grep -q 'Machine:.*AArch64'
if LC_ALL=C grep -aq -- '--image' "$out/m4-apfs-reader"; then
  echo 'Refusing a helper with fixture input support' >&2
  exit 1
fi
(
  cd "$out"
  sha256sum m4-apfs-reader > SHA256SUMS
  readelf -h -d --version-info m4-apfs-reader > elf.txt
)
# Retain this and the CMake compiler-identification log with the build receipt.
sha256sum "$here/CMakeLists.txt" "$here/helper.cpp" "$here/apfslib-readonly.patch" > "$out/build-inputs.sha256"
cmake --version > "$out/cmake-version.txt"
cat "$out/SHA256SUMS"
