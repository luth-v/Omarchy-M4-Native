#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Luthfi Hizb
# Build both camera modules from source, offline, without touching the kernel tree.
set -euo pipefail
if [ "$#" != 4 ]; then
  echo 'usage: build-j713-camera.sh KERNEL_BUILD MEDIA_EXPORTS IMAGE_ID OUTPUT' >&2
  exit 2
fi
kernel=$(realpath "$1")
exports=$(realpath "$2")
image=$3
output=$(realpath -m "$4")
source_dir=$(cd "$(dirname "$0")/../video/j713/camera" && pwd)
[[ $image =~ ^sha256:[a-f0-9]{64}$ ]]
[ ! -e "$output" ]
(cd "$kernel" && sha256sum --check "$source_dir/kernel-inputs.sha256")
[ "$(sha256sum "$exports" | cut -d' ' -f1)" = ae0764effc35075fedb4e3492d973a655e963e612cba1e28a1609b6d4f876285 ]
mkdir -p "$output"
cp -a "$source_dir/kernel" "$output/kernel"
cp "$exports" "$output/media.symvers"
# Avoid stale generated files in a source checkout.
find "$output/kernel" -type f \( -name '*.o' -o -name '*.ko' -o -name '*.cmd' -o -name '*.mod*' -o -name '*.dtbo' -o -name Module.symvers -o -name modules.order \) -delete
docker run --rm --network none --pull never --entrypoint /bin/bash \
  --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$kernel:/kernel:ro" -v "$output:/build" -w /build/kernel "$image" -eu -c '
    clang --version | head -1
    make -s -C /kernel ARCH=arm64 LLVM=1 LOCALVERSION= KCFLAGS=-Werror W=1 \
      KBUILD_EXTRA_SYMBOLS=/build/media.symvers M=/build/kernel -j12 modules
    for module in /build/kernel/*.ko; do
      test "$(modinfo -F vermagic "$module" | cut -d " " -f1)" = 7.1.9-m4-gpu-backport-20261008
      modinfo -F vermagic "$module"
      llvm-readelf -h "$module" | grep "Machine:.*AArch64"
    done'
(cd "$kernel" && sha256sum --check "$source_dir/kernel-inputs.sha256")
sha256sum "$output"/kernel/*.ko
