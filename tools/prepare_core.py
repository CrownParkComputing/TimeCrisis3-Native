#!/usr/bin/env python3
"""Export the pinned upstream commit locally, without using working-tree changes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkout", type=Path, required=True)
    args = parser.parse_args()
    lock = json.loads((ROOT / "third_party/pcsx2.lock.json").read_text())
    commit = subprocess.check_output(
        ["git", "-C", str(args.checkout), "rev-parse", lock["commit"] + "^{commit}"], text=True
    ).strip()
    if commit != lock["commit"]:
        raise SystemExit("Pinned commit mismatch")
    target = ROOT / ".local/pcsx2-source"
    if target.exists():
        raise SystemExit(f"Refusing to replace existing source: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent, prefix="core-export-") as temporary:
        temporary = Path(temporary)
        archive = temporary / "source.tar"
        subprocess.run(["git", "-C", str(args.checkout), "archive", "--format=tar",
                        "--output=" + str(archive), commit], check=True)
        with archive.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        export = temporary / "source"
        export.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(export, filter="data")
        if not (export / lock["license_file"]).is_file():
            raise SystemExit("Missing upstream license")
        (export / "tc3-source-provenance.json").write_text(json.dumps(
            {**lock, "git_archive_sha256": digest, "local_patches": []}, indent=2) + "\n")
        export.rename(target)
    print(target)


if __name__ == "__main__":
    main()
