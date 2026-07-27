#!/usr/bin/env python3
"""Guard OfflineCore's ARM64 HTTP proxy against non-RPC callback targets."""

from __future__ import annotations

import argparse
import hashlib
import pathlib


EXPECTED_SHA256 = "347d7fd0ddb2d705540475fb4f456f0301a8643685a52b737c5645941fdfb7f3"

PATCHES = {
    # rpc_execute_proxy: retain the currently executing RPC object in the
    # no-longer-needed hook-stub slot at image base + 0x13d640.
    0x94E18: (
        "480500b0081d43f9680200b401000014480500b0081d43f9"
        "e01b40f9e11740f900013fd6",
        "490500b0281d43f9680200b4e01b40f9202103f91f2003d5"
        "1f2003d5e11740f900013fd6",
    ),
    # read_pointer_field: when reading Delegate.m_target (+0x20), return null
    # unless it matches the RPC object observed above. Existing HTTP fallback
    # logic handles the resulting empty service/method without interception.
    0x95F4C: (
        "ff8300d1e00b00f9e10700f9e80b40f9880000b501000014"
        "ff0f00f906000014e80b40f9e90740f9086969f8e80f00f9"
        "01000014e00f40f9ff830091c0035fd6",
        "200100b4006861f83f8000f1a100005448050090082143f9"
        "1f0008eb00009f9ac0035fd6e0031faac0035fd61f2003d5"
        "1f2003d51f2003d51f2003d51f2003d5",
    ),
}


def patch(input_path: pathlib.Path, output_path: pathlib.Path) -> None:
    data = bytearray(input_path.read_bytes())
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256:
        raise ValueError(f"Unexpected ARM64 libofflinecore.so SHA-256: {digest}")

    for offset, (expected_hex, replacement_hex) in PATCHES.items():
        expected = bytes.fromhex(expected_hex)
        replacement = bytes.fromhex(replacement_hex)
        if len(expected) != len(replacement):
            raise AssertionError(f"Patch length mismatch at {offset:#x}")
        actual = data[offset : offset + len(expected)]
        if actual != expected:
            raise ValueError(f"Unexpected bytes at file offset {offset:#x}")
        data[offset : offset + len(expected)] = replacement

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(data)
    print(f"Wrote guarded ARM64 OfflineCore: {output_path}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_library", type=pathlib.Path)
    parser.add_argument("output_library", type=pathlib.Path)
    args = parser.parse_args()
    patch(args.input_library.resolve(), args.output_library.resolve())


if __name__ == "__main__":
    main()
