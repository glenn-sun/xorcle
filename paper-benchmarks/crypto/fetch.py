#!/usr/bin/env python3
"""Fetch Julian Danner's distributed benchmark files and write `instances.csv`.

    fetch.py

The instances are the release `benchmark_files` of j-danner/xnf_sat_solving (the
files of Danner's dissertation), less its random XNF families (rand_2xnf_sat,
rand_2xnf_unsat, rand_l2xnf_ld); rand_qp is kept. Its ascon, bivium, ctc and
rand_qp zips are byte-identical to those of j-danner/Xorricane-paper's release of
the same name. Every zip is pinned by sha256. The files carry no licence, so they
are downloaded, never committed.

Each instance is used as `.xnf`, `.2xnf`, `.cnf` (XORs cut and blasted) and `.anf`.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import sys
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
INSTANCES = HERE / "instances"
DOWNLOAD = INSTANCES / "download"

RELEASE = "https://github.com/j-danner/xnf_sat_solving/releases/download/benchmark_files/"
FAMILIES = {
    "ascon": "98fbf3d5bef30ba46cfcf1f12148459b1edb92f591329e8d8db5d7f3d6f8cf58",
    "bivium": "12ae8154e8ec26e84e4a19d2cbc50fe266bb21b8e51fa147c9935d5f6b0f7d07",
    "ctc": "88639467b56be5c87ad3264b1417535b00c8108501b5b8247253b5d09bc56446",
    "prince": "40d388618fad873c7d63c1c38346772ab6dcb67aebbe311cf624aa23bf0c642b",
    "rand_qp_type_I": "28f2b6d3716c42eb1f84aecade465d0854b5c9fc1568718bd45934b09cb76b04",
    "rand_qp_type_IV": "2f7a3925f7832cdff3893a8448741cf66b19569f2859b56df70a41453c0ec2c8",
    "simon": "a7c589363f7906ce74d3f48010264f8ac6d6bc66367bdd4ec74516280300e7d5",
}

EXTS = (".xnf", ".2xnf", ".cnf", ".anf")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(url: str, dst: Path, digest: str) -> None:
    if dst.is_file() and sha256(dst) == digest:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    print(f"downloading {url}", flush=True)
    tmp = dst.with_suffix(dst.suffix + ".part")
    with urllib.request.urlopen(url) as r, tmp.open("wb") as f:
        while block := r.read(1 << 20):
            f.write(block)
    got = sha256(tmp)
    if got != digest:
        raise SystemExit(f"{url}: sha256 {got}, expected {digest}")
    tmp.rename(dst)


def unpack(family: str) -> None:
    out = INSTANCES / family
    zpath = DOWNLOAD / "xnf_sat_solving" / f"{family}.zip"
    with zipfile.ZipFile(zpath) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        if out.is_dir() and all((out / Path(n).name).is_file() for n in names):
            return
        print(f"unpacking {family}", flush=True)
        out.mkdir(parents=True, exist_ok=True)
        for n in names:
            (out / Path(n).name).write_bytes(z.read(n))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.parse_args()

    for family, digest in FAMILIES.items():
        fetch(RELEASE + f"{family}.zip", DOWNLOAD / "xnf_sat_solving" / f"{family}.zip", digest)
        unpack(family)

    rows = []
    for family in FAMILIES:
        for xnf in sorted((INSTANCES / family).glob("*.xnf")):
            stem = xnf.with_suffix("")
            for ext in EXTS:
                if not stem.with_suffix(ext).is_file():
                    raise SystemExit(f"{stem}{ext} missing")
            rows.append({"family": family, "name": stem.name,
                         "dir": str(stem.parent.relative_to(HERE)), "xnf_sha256": sha256(xnf)})
    with (HERE / "instances.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["family", "name", "dir", "xnf_sha256"])
        w.writeheader()
        w.writerows(rows)
    print(f"instances.csv: {len(rows)} instances", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
