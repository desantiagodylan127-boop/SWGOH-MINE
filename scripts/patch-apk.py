#!/usr/bin/env python3
"""Replace ShadowHook in an offline6 APK without redistributing game assets."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import tempfile
import zipfile


REPLACEMENTS = {
    "lib/arm64-v8a/libshadowhook.so": "arm64-v8a",
}


def is_old_signature(name: str) -> bool:
    upper = name.upper()
    if not upper.startswith("META-INF/"):
        return False
    return upper == "META-INF/MANIFEST.MF" or upper.endswith((".SF", ".RSA", ".DSA", ".EC"))


def patch_apk(source: pathlib.Path, native_dir: pathlib.Path, output: pathlib.Path) -> None:
    replacements = {
        apk_path: native_dir / abi / "libshadowhook.so"
        for apk_path, abi in REPLACEMENTS.items()
    }
    missing = [str(path) for path in replacements.values() if not path.is_file()]
    if missing:
        raise FileNotFoundError(f"Missing built libraries: {', '.join(missing)}")

    with tempfile.NamedTemporaryFile(
        prefix=f"{output.stem}-", suffix=".apk", dir=output.parent, delete=False
    ) as temporary:
        temporary_path = pathlib.Path(temporary.name)

    try:
        found: set[str] = set()
        with zipfile.ZipFile(source, "r") as source_zip, zipfile.ZipFile(
            temporary_path, "w", allowZip64=True
        ) as output_zip:
            output_zip.comment = source_zip.comment
            for info in source_zip.infolist():
                if is_old_signature(info.filename):
                    continue
                if info.filename in replacements:
                    found.add(info.filename)
                    data = replacements[info.filename].read_bytes()
                    replacement_info = zipfile.ZipInfo(info.filename, info.date_time)
                    replacement_info.compress_type = zipfile.ZIP_STORED
                    replacement_info.external_attr = info.external_attr
                    replacement_info.create_system = info.create_system
                    output_zip.writestr(replacement_info, data)
                else:
                    output_zip.writestr(info, source_zip.read(info.filename))

        absent = set(REPLACEMENTS) - found
        if absent:
            raise ValueError(f"Input is not an offline6 universal APK; absent: {sorted(absent)}")
        shutil.move(temporary_path, output)
    finally:
        temporary_path.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_apk", type=pathlib.Path)
    parser.add_argument("output_apk", type=pathlib.Path)
    parser.add_argument(
        "--native-dir",
        type=pathlib.Path,
        default=pathlib.Path("dist/jni"),
        help="Directory containing <abi>/libshadowhook.so (default: dist/jni)",
    )
    args = parser.parse_args()

    args.output_apk.parent.mkdir(parents=True, exist_ok=True)
    patch_apk(args.input_apk.resolve(), args.native_dir.resolve(), args.output_apk.resolve())
    print(f"Wrote unsigned, unaligned APK: {args.output_apk}")


if __name__ == "__main__":
    main()
