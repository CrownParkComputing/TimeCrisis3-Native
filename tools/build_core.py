#!/usr/bin/env python3
"""Build the pinned compatibility library without the generic Qt frontend."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--prefix", type=Path, help="Optional locally supplied dependency prefix")
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--native-block-proof", action="store_true",
                   help="Enable the optional verified-block integration patch")
    p.add_argument("--playable", action="store_true", help="Build the linked SDL3 TC3 executable")
    args = p.parse_args()
    if args.jobs < 1:
        p.error("--jobs must be positive")
    source, build = ROOT / ".local/pcsx2-source", ROOT / ".local/core-build"
    lock = json.loads((ROOT / "third_party/pcsx2.lock.json").read_text())
    provenance = json.loads((source / "tc3-source-provenance.json").read_text())
    if provenance["commit"] != lock["commit"]:
        raise SystemExit("Source provenance does not match pinned core")
    if args.native_block_proof and "TC3_NATIVE_BLOCK_PROOF_ROOT" not in (
            source / "pcsx2/CMakeLists.txt").read_text():
        raise SystemExit("Apply runtime/pcsx2-native-block-proof.patch before enabling its build")
    command = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang",
               "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
               "-DENABLE_QT_UI=OFF", "-DENABLE_TESTS=OFF", "-DENABLE_GSRUNNER=OFF",
               "-DCMAKE_DISABLE_FIND_PACKAGE_FFMPEG=ON"]
    if args.prefix:
        command.append("-DCMAKE_PREFIX_PATH=" + str(args.prefix.resolve()))
    command.append("-DTC3_NATIVE_BLOCK_PROOF_ROOT=" +
                   (str(ROOT / "runtime") if args.native_block_proof else ""))
    command.append("-DCMAKE_PROJECT_Pcsx2_INCLUDE=" +
                   (str(ROOT / "host/inject_playable.cmake") if args.playable else ""))
    subprocess.run(command, check=True)
    subprocess.run(["cmake", "--build", str(build), "--target",
                    "tc3-native" if args.playable else "PCSX2", "-j", str(args.jobs)], check=True)
    library = build / "pcsx2/libpcsx2.a"
    with library.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    report = {"source": provenance, "configure_command": command,
              "library": str(library), "sha256": digest,
              "validation": "static library built; gameplay not tested"}
    if args.native_block_proof:
        report["native_proof_sources"] = {
            name: hashlib.sha256((ROOT / "runtime" / name).read_bytes()).hexdigest()
            for name in ("native_block_provider.cpp", "native_block_provider.h",
                         "pcsx2_block_bridge.cpp", "pcsx2_block_bridge.h")}
    if args.playable:
        binary = build / "bin/tc3-native"
        with binary.open("rb") as stream:
            report["executable"] = {"path": str(binary),
                "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}
    (build / "tc3-build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"library": str(library), "sha256": digest}))


if __name__ == "__main__":
    main()
