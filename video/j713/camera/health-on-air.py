#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Luthfi Hizb
"""Read-only host check: compare cached executable bytes with direct disk reads."""
import hashlib
import json
import mmap
import os
from pathlib import Path


def direct_digest(path):
    block = max(16384, os.sysconf("SC_PAGE_SIZE"))
    digest = hashlib.sha256()
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECT)
    buffer = mmap.mmap(-1, block)
    try:
        while True:
            count = os.readv(descriptor, [buffer])
            if not count:
                break
            digest.update(buffer[:count])
            if count < block:
                break
    finally:
        buffer.close()
        os.close(descriptor)
    return digest.hexdigest()


def main():
    libraries = sorted(Path("/usr/lib/systemd").glob("libsystemd-shared-*.so"))
    if not libraries:
        raise SystemExit("No systemd shared library found; host check cannot run.")
    paths = [*libraries, Path("/usr/lib/libc.so.6"), Path("/usr/bin/bash"),
             Path("/usr/bin/sudo"), Path("/usr/bin/systemctl")]
    good = True
    print(json.dumps({"boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}))
    for path in paths:
        try:
            before = path.stat()
            cached = hashlib.sha256(path.read_bytes()).hexdigest()
            direct = direct_digest(path)
            after = path.stat()
            stable = (before.st_ino, before.st_size, before.st_mtime_ns) == (
                after.st_ino, after.st_size, after.st_mtime_ns)
            match = cached == direct and stable
            print(json.dumps({"path": str(path), "cached_sha256": cached,
                              "direct_sha256": direct, "stable": stable, "match": match}))
            good &= match
        except OSError as error:
            print(json.dumps({"path": str(path), "error": str(error), "match": False}))
            good = False
    raise SystemExit(0 if good else 1)


if __name__ == "__main__":
    main()
