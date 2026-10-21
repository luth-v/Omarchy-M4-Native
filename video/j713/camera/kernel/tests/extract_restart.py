# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production restart and lifecycle functions against offline kernel stubs."""
from pathlib import Path
import re

driver = Path(__file__).resolve().parents[1]
headers = {
    "restart_impl.h": (("j713_cam_main.c", ("cam_restart_ms_set", "cam_rearm", "cam_fw_stop")),
                       ("j713_cam_fw.c", ("cam_idle_work",))),
    # Callees first, so the test needs no prototypes.
    "lifecycle_impl.h": (("j713_cam_main.c", ("cam_fail",)),
                         ("j713_cam_fw.c", ("cam_failf", "cam_lifecycle", "cam_lifecycle_work",
                                            "cam_session_start")),
                         ("j713_cam_main.c", ("cam_boot", "cam_fw_stop", "cam_fw_sleep",
                                              "cam_shutdown")),
                         ("j713_cam_v4l2.c", ("cam_start_streaming", "cam_stop_streaming"))),
}
for header, functions in headers.items():
    output = ["/* Generated from the production sources; do not edit. */\n"]
    for filename, names in functions:
        source = (driver / filename).read_text()
        for name in names:
            match = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^;]+?\n\{\n.*?^}\n",
                              source, re.MULTILINE | re.DOTALL)
            if match is None:
                raise RuntimeError(f"Cannot extract {filename}:{name}")
            output.append(match.group())
    (driver / "tests/.build" / header).write_text("\n".join(output))
# Both restart timings default to 0: a static without an initializer.
main = (driver / "j713_cam_main.c").read_text()
for name in ("restart_settle_ms", "restart_off_ms"):
    if not re.search(r"^static unsigned int " + name + r"(?: = 0)?;$", main, re.MULTILINE):
        raise RuntimeError(f"j713_cam_main.c: {name} must default to 0")
print("restart timing defaults: restart_settle_ms=0 restart_off_ms=0")
