#!/usr/bin/env python3
"""Write the ESP Web Tools manifests for the web flasher, then check them.

Called by .github/workflows/pages.yml against the directory holding the merged
images already downloaded from a release:

    build_flasher_manifests.py site/firmware 1.16.1

A manifest that names a file which is not there, or that does not parse, fails
silently in the browser as a button that appears to do nothing. That is worth
catching in CI rather than in someone else's hands, so every manifest is read
back and its firmware confirmed present and plausibly sized.
"""

import json
import sys
from pathlib import Path

# profile -> (label shown while flashing, asset name suffix)
#
# Both entries are full-flash images written at offset 0, so each manifest needs
# a single part and nothing here has to track partitions.csv. The two builds
# differ only in the status LED pin; see .github/workflows/build.yml.
PROFILES = {
    "default": ("ESP32-S3-Zero", "merged.bin"),
    "n8r8": ("ESP32-S3-DevKitC-1", "n8r8-merged.bin"),
}

# A merged image is ~1.2MB. Anything far below that is a truncated download or
# an error page saved under a .bin name.
MIN_PLAUSIBLE_BYTES = 500_000


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <firmware-dir> <version>", file=sys.stderr)
        return 2

    firmware_dir, version = Path(argv[1]), argv[2]

    for profile, (label, suffix) in PROFILES.items():
        manifest = {
            "name": f"hms-esp-apc ({label})",
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [
                {
                    "chipFamily": "ESP32-S3",
                    "parts": [
                        {"path": f"hms-esp-apc-{version}-{suffix}", "offset": 0}
                    ],
                }
            ],
        }
        path = firmware_dir / f"manifest-{profile}.json"
        path.write_text(json.dumps(manifest, indent=2) + "\n")

    return verify(firmware_dir)


def verify(firmware_dir: Path) -> int:
    """Read every manifest back the way the browser will, and prove it resolves."""
    failed = False

    manifests = sorted(firmware_dir.glob("manifest-*.json"))
    if len(manifests) != len(PROFILES):
        print(
            f"::error::wrote {len(PROFILES)} manifests but found {len(manifests)}"
        )
        return 1

    for path in manifests:
        try:
            manifest = json.loads(path.read_text())
        except json.JSONDecodeError as exc:
            print(f"::error file={path}::invalid JSON: {exc}")
            failed = True
            continue

        for build in manifest["builds"]:
            for part in build["parts"]:
                binary = firmware_dir / part["path"]
                if not binary.is_file():
                    print(f"::error file={path}::missing firmware {part['path']}")
                    failed = True
                    continue
                size = binary.stat().st_size
                print(f"{path.name} -> {part['path']} ({size:,} bytes)")
                if size < MIN_PLAUSIBLE_BYTES:
                    print(
                        f"::error file={path}::{part['path']} is only {size} bytes, "
                        "which is too small to be a merged image"
                    )
                    failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
