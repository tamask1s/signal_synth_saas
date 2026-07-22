#!/usr/bin/env python3
"""Adopt the clean sibling signal_synth HEAD and its curated release set.

The workflow has intentionally no arbitrary command or path arguments. It
updates the exact producer and catalog pins already tracked by this repository,
builds a fresh core CLI serially, verifies its embedded identity, and performs
a clean curated-pack import.
"""

import json
import os
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE = ROOT.parent / "signal_synth"
CORE_BUILD = ROOT / "build" / "signal_synth_live"
PIN_FILE = ROOT / "CMakeLists.txt"
CATALOG_FILE = ROOT / "packs" / "curated_pack_metadata_v1.catalog"
CORE_METADATA = CORE / "examples" / "catalog" / "curated_pack_metadata_v1.json"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^sha256:[0-9a-f]{64}$")


def run(command, cwd=ROOT):
    subprocess.run([str(value) for value in command], cwd=str(cwd), check=True)


def output(command, cwd=ROOT):
    return subprocess.check_output(
        [str(value) for value in command], cwd=str(cwd), universal_newlines=True
    ).strip()


def read_json(path):
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def require_clean(repo, name):
    if output(["git", "status", "--porcelain"], repo):
        raise RuntimeError("{} checkout must be clean before adoption".format(name))


def current_core_pin():
    text = PIN_FILE.read_text(encoding="utf-8")
    match = re.search(
        r'SYN_SIG_RA_EXPECTED_SIGNAL_SYNTH_COMMIT\s+"([0-9a-f]{40})"', text
    )
    if not match:
        raise RuntimeError("unable to read the current core pin from CMakeLists.txt")
    return match.group(1)


def tracked_files_containing(value):
    result = subprocess.run(
        ["git", "grep", "-l", "-F", "--", value],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    if result.returncode not in (0, 1):
        raise RuntimeError(result.stderr.strip() or "git grep failed")
    return [ROOT / line for line in result.stdout.splitlines() if line]


def replace_tracked(old, new, label):
    if old == new:
        return 0
    paths = tracked_files_containing(old)
    if not paths:
        raise RuntimeError("the old {} pin is not present in tracked files".format(label))
    old_bytes = old.encode("ascii")
    new_bytes = new.encode("ascii")
    count = 0
    for path in paths:
        content = path.read_bytes()
        occurrences = content.count(old_bytes)
        if occurrences:
            path.write_bytes(content.replace(old_bytes, new_bytes))
            count += occurrences
    return count


def positive_build_jobs():
    value = os.environ.get("BUILD_JOBS", "1")
    if not value.isdigit() or int(value) < 1:
        raise RuntimeError("BUILD_JOBS must be a positive integer")
    return value


def main():
    if not (CORE / "CMakeLists.txt").is_file() or not CORE_METADATA.is_file():
        raise RuntimeError("the sibling ../signal_synth checkout is incomplete")
    require_clean(ROOT, "signal_synth_saas")
    require_clean(CORE, "signal_synth")

    old_core = current_core_pin()
    new_core = output(["git", "rev-parse", "HEAD"], CORE)
    if not HEX40.fullmatch(new_core):
        raise RuntimeError("signal_synth HEAD is not a full Git commit")

    old_catalog = read_json(CATALOG_FILE)["source_catalog_sha256"]
    new_catalog = read_json(CORE_METADATA)["source_catalog_sha256"]
    if not SHA256.fullmatch(old_catalog) or not SHA256.fullmatch(new_catalog):
        raise RuntimeError("curated catalog SHA-256 is malformed")

    core_replacements = replace_tracked(old_core, new_core, "core commit")
    catalog_replacements = replace_tracked(
        old_catalog, new_catalog, "curated catalog"
    )

    jobs = positive_build_jobs()
    run([
        "cmake", "-S", CORE, "-B", CORE_BUILD,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG",
        "-DSIGNAL_SYNTH_BUILD_TESTS=OFF",
        "-DSIGNAL_SYNTH_BUILD_CLI=ON",
    ])
    run([
        "cmake", "--build", CORE_BUILD, "--target", "signal_synth_cli",
        "-j{}".format(jobs),
    ])
    cli = CORE_BUILD / "signal-synth"
    contract = json.loads(output([cli, "contract"]))
    generator = contract.get("generator", {})
    if generator.get("git_commit") != new_core:
        raise RuntimeError("fresh core CLI does not embed signal_synth HEAD")
    if generator.get("build_identity") != "signal_synth/" + new_core:
        raise RuntimeError("fresh core CLI build identity is inconsistent")

    run([
        sys.executable,
        ROOT / "scripts" / "import_curated_release_set.py",
        "--metadata", CORE_METADATA,
        "--source-root", CORE,
        "--out", ROOT / "packs",
        "--signal-synth-cli", cli,
        "--clean",
    ])

    imported = read_json(CATALOG_FILE)
    if imported.get("source_catalog_sha256") != new_catalog:
        raise RuntimeError("imported catalog does not match the core release set")
    if tracked_files_containing(old_core) and old_core != new_core:
        raise RuntimeError("stale core commit pins remain after adoption")
    if tracked_files_containing(old_catalog) and old_catalog != new_catalog:
        raise RuntimeError("stale catalog pins remain after adoption")
    run(["git", "diff", "--check"])

    print("adopted signal_synth {} -> {}".format(old_core, new_core))
    print("adopted catalog {} -> {}".format(old_catalog, new_catalog))
    print("updated {} commit and {} catalog pin occurrences".format(
        core_replacements, catalog_replacements
    ))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print("adopt_core_release: {}".format(error), file=sys.stderr)
        raise SystemExit(1)
