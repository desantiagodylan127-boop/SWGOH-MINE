#!/usr/bin/env python3
"""Package the source-built ARM64 bridge into an offline Heroes APK shell."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import tempfile
import zipfile


CORE_PATH = "lib/arm64-v8a/libofflinecore.so"
OBSOLETE_HOOK_PATHS = (
    "lib/arm64-v8a/libshadowhook.so",
    "lib/armeabi-v7a/libshadowhook.so",
)


def is_signature(name: str) -> bool:
    upper = name.upper()
    return upper.startswith("META-INF/") and (
        upper == "META-INF/MANIFEST.MF"
        or upper.endswith((".SF", ".RSA", ".DSA", ".EC"))
    )


def package(source: pathlib.Path, core: pathlib.Path, output: pathlib.Path) -> None:
    if not core.is_file():
        raise FileNotFoundError(f"Source-built core not found: {core}")
    replacement = core.read_bytes()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        prefix=f"{output.stem}-", suffix=".apk", dir=output.parent, delete=False
    ) as temporary:
        temporary_path = pathlib.Path(temporary.name)

    found_core = False
    try:
        with zipfile.ZipFile(source, "r") as incoming, zipfile.ZipFile(
            temporary_path, "w", allowZip64=True
        ) as outgoing:
            outgoing.comment = incoming.comment
            for info in incoming.infolist():
                if is_signature(info.filename):
                    continue
                if info.filename in OBSOLETE_HOOK_PATHS:
                    continue
                if info.filename == CORE_PATH:
                    found_core = True
                    replacement_info = zipfile.ZipInfo(info.filename, info.date_time)
                    replacement_info.compress_type = zipfile.ZIP_STORED
                    replacement_info.external_attr = info.external_attr
                    replacement_info.create_system = info.create_system
                    outgoing.writestr(replacement_info, replacement)
                else:
                    outgoing.writestr(info, incoming.read(info.filename))
        if not found_core:
            raise ValueError(f"Input APK is missing {CORE_PATH}")
        shutil.move(temporary_path, output)
    finally:
        temporary_path.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_apk", type=pathlib.Path)
    parser.add_argument("output_apk", type=pathlib.Path)
    parser.add_argument(
        "--core",
        type=pathlib.Path,
        default=pathlib.Path("dist/legacy/arm64-v8a/libofflinecore.so"),
    )
    arguments = parser.parse_args()
    package(
        arguments.input_apk.resolve(),
        arguments.core.resolve(),
        arguments.output_apk.resolve(),
    )
    print(f"Wrote unsigned source-bridge APK: {arguments.output_apk}")


if __name__ == "__main__":
    main()
