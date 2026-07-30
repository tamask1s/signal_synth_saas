#!/usr/bin/env python3
"""Adopt the clean, pushed sibling signal_synth HEAD as one transaction.

The workflow has no arbitrary command or path arguments. It builds and
validates the new producer first, imports packs into a staging directory, and
only then updates tracked SaaS files. Successful runs are quiet; failures show
the last useful log lines and restore the original checkout.
"""

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE = ROOT.parent / "signal_synth"
CORE_BUILD = ROOT / "build" / "signal_synth_live"
PIN_FILE = ROOT / "CMakeLists.txt"
SAAS_CONTRACT_SOURCE = ROOT / "src" / "integration" / "core_contract.cpp"
PACK_ROOT = ROOT / "packs"
CATALOG_FILE = PACK_ROOT / "curated_pack_metadata_v1.catalog"
CORE_METADATA = CORE / "examples" / "catalog" / "curated_pack_metadata_v1.json"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^sha256:[0-9a-f]{64}$")
VERSION = re.compile(r"^version = ([0-9]+\.[0-9]+\.[0-9]+)$", re.MULTILINE)
CONTRACT_CONSTANT = re.compile(
    r'const char k([A-Za-z0-9]+)\[\]\s*=\s*"([^"]+)";'
)
CONTRACT_KEYS = {
    "Integration": ("contract",),
    "ChallengePackage": ("contracts", "challenge_package"),
    "ScoringManifest": ("contracts", "scoring_manifest"),
    "VerificationProtocol": ("contracts", "verification_protocol"),
    "EvidenceProfiles": ("contracts", "evidence_profiles"),
    "Submission": ("contracts", "submission"),
    "SubmissionFormats": ("contracts", "submission_formats"),
    "MeasurementValues": ("contracts", "measurement_values"),
    "MeasurementTruth": ("contracts", "measurement_truth"),
    "MeasurementScoring": ("contracts", "measurement_scoring"),
    "LocalVerification": ("contracts", "local_verification"),
    "Authoring": ("contracts", "scenario_authoring"),
    "Templates": ("contracts", "scenario_templates"),
    "ExternalNoiseTruth": ("contracts", "external_noise_truth"),
}
VERIFIER_VERSION_FILES = tuple(
    ROOT / value for value in (
        "README.md",
        "doc/API_GUIDE.md",
        "doc/PACK_CATALOG.md",
        "doc/PRODUCT_CAPABILITIES.md",
        "doc/openapi.yaml",
        "scripts/verify_live.sh",
        "test/integration/e2e_smoke.sh",
        "test/unit/mcp_server_test.cpp",
        "test/unit/route_test.cpp",
        "test/unit/worker_test.cpp",
    )
)
CATALOG_VERSION_FILES = tuple(
    ROOT / value for value in (
        "README.md",
        "doc/PACK_CATALOG.md",
        "doc/openapi.yaml",
        "scripts/verify_live.sh",
        "src/catalog/pack_catalog.cpp",
        "test/unit/mcp_server_test.cpp",
        "test/unit/pack_catalog_test.cpp",
        "test/unit/worker_test.cpp",
    )
)


def command_text(command):
    return " ".join(str(value) for value in command)


def run(command, cwd=ROOT):
    result = subprocess.run(
        [str(value) for value in command],
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
    )
    if result.returncode:
        tail = "\n".join(result.stdout.splitlines()[-80:])
        raise RuntimeError(
            "command failed ({}): {}\n{}".format(
                result.returncode, command_text(command), tail
            ).rstrip()
        )
    return result.stdout


def output(command, cwd=ROOT):
    return run(command, cwd).strip()


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


def verifier_version(setup_text, label):
    match = VERSION.search(setup_text)
    if not match:
        raise RuntimeError("unable to read {} verifier version".format(label))
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


def current_contract_tokens():
    text = SAAS_CONTRACT_SOURCE.read_text(encoding="utf-8")
    constants = dict(CONTRACT_CONSTANT.findall(text))
    missing = sorted(set(CONTRACT_KEYS) - set(constants))
    if missing:
        raise RuntimeError(
            "unable to read SaaS contract constants: {}".format(
                ", ".join(missing)
            )
        )
    return constants


def nested_value(document, path):
    value = document
    for key in path:
        if not isinstance(value, dict) or key not in value:
            raise RuntimeError(
                "new core contract has no {}".format(".".join(path))
            )
        value = value[key]
    if not isinstance(value, str) or not value.startswith("synsigra_"):
        raise RuntimeError(
            "new core contract token is invalid at {}".format(".".join(path))
        )
    return value


def contract_replacements(new_contract):
    current = current_contract_tokens()
    changes = []
    for constant, path in CONTRACT_KEYS.items():
        old = current[constant]
        new = nested_value(new_contract, path)
        if old != new:
            changes.append((old, new, ".".join(path)))
    return changes


def operational_contract_paths(value):
    paths = []
    for path in tracked_files_containing(value):
        relative = path.relative_to(ROOT).as_posix()
        if relative.startswith("packs/"):
            continue
        if relative.startswith("doc/20260724_user_suggestions/"):
            continue
        if relative == "doc/synsigra_report_improvements.md":
            continue
        paths.append(path)
    return paths


def replace_paths(paths, old, new, label, required=True):
    if old == new:
        return 0
    old_bytes = old.encode("ascii")
    new_bytes = new.encode("ascii")
    count = 0
    for path in paths:
        content = path.read_bytes()
        occurrences = content.count(old_bytes)
        if occurrences:
            path.write_bytes(content.replace(old_bytes, new_bytes))
            count += occurrences
    if required and not count:
        raise RuntimeError("the old {} pin is not present".format(label))
    return count


def replace_numeric_token(paths, old, new, label):
    if old == new:
        return 0
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", old) or not re.fullmatch(
            r"[0-9]+(?:\.[0-9]+)+", new):
        raise RuntimeError("{} is not a dotted numeric version".format(label))
    pattern = re.compile(
        rb"(?<![0-9])" + re.escape(old.encode("ascii")) + rb"(?![0-9])"
    )
    replacement = new.encode("ascii")
    count = 0
    for path in paths:
        content = path.read_bytes()
        updated, occurrences = pattern.subn(replacement, content)
        if occurrences:
            path.write_bytes(updated)
            count += occurrences
    if not count:
        raise RuntimeError("the old {} token is not present".format(label))
    return count


def positive_build_jobs():
    value = os.environ.get("BUILD_JOBS", "1")
    if not value.isdigit() or int(value) < 1:
        raise RuntimeError("BUILD_JOBS must be a positive integer")
    return value


def restore_checkout(file_backups, pack_backup):
    if PACK_ROOT.exists():
        shutil.rmtree(str(PACK_ROOT))
    shutil.copytree(str(pack_backup), str(PACK_ROOT))
    for path, content in file_backups.items():
        if PACK_ROOT not in path.parents:
            path.write_bytes(content)


def main():
    if not (CORE / "CMakeLists.txt").is_file() or not CORE_METADATA.is_file():
        raise RuntimeError("the sibling ../signal_synth checkout is incomplete")
    require_clean(ROOT, "signal_synth_saas")
    require_clean(CORE, "signal_synth")

    old_core = current_core_pin()
    new_core = output(["git", "rev-parse", "HEAD"], CORE)
    if not HEX40.fullmatch(new_core):
        raise RuntimeError("signal_synth HEAD is not a full Git commit")
    if old_core == new_core:
        raise RuntimeError("signal_synth HEAD is already adopted")
    if output(["git", "rev-parse", "origin/master"], CORE) != new_core:
        raise RuntimeError("signal_synth HEAD must be pushed to origin/master first")

    old_catalog_metadata = read_json(CATALOG_FILE)
    new_catalog_metadata = read_json(CORE_METADATA)
    old_catalog = old_catalog_metadata["source_catalog_sha256"]
    new_catalog = new_catalog_metadata["source_catalog_sha256"]
    old_catalog_version = old_catalog_metadata["catalog_version"]
    new_catalog_version = new_catalog_metadata["catalog_version"]
    if not SHA256.fullmatch(old_catalog) or not SHA256.fullmatch(new_catalog):
        raise RuntimeError("curated catalog SHA-256 is malformed")
    old_setup = output(["git", "show", old_core + ":setup.cfg"], CORE)
    new_setup = (CORE / "setup.cfg").read_text(encoding="utf-8")
    old_verifier = verifier_version(old_setup, "old core")
    new_verifier = verifier_version(new_setup, "new core")

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
    token_changes = contract_replacements(contract)

    with tempfile.TemporaryDirectory(prefix="synsigra-core-adopt-") as temporary:
        temporary_root = pathlib.Path(temporary)
        staged_packs = temporary_root / "packs"
        pack_backup = temporary_root / "old-packs"
        run([
            sys.executable,
            ROOT / "scripts" / "import_curated_release_set.py",
            "--metadata", CORE_METADATA,
            "--source-root", CORE,
            "--out", staged_packs,
            "--signal-synth-cli", cli,
            "--clean",
        ])
        if read_json(staged_packs / CATALOG_FILE.name).get(
                "source_catalog_sha256") != new_catalog:
            raise RuntimeError("staged catalog does not match the core release set")

        core_paths = tracked_files_containing(old_core)
        catalog_paths = tracked_files_containing(old_catalog)
        contract_paths = []
        for old, _new, _label in token_changes:
            contract_paths.extend(operational_contract_paths(old))
        changed_paths = set(
            core_paths + catalog_paths + list(VERIFIER_VERSION_FILES) +
            list(CATALOG_VERSION_FILES) + contract_paths
        )
        file_backups = dict((path, path.read_bytes()) for path in changed_paths)
        shutil.copytree(str(PACK_ROOT), str(pack_backup))
        try:
            core_replacements = replace_paths(
                core_paths, old_core, new_core, "core commit")
            catalog_replacements = replace_paths(
                catalog_paths, old_catalog, new_catalog, "curated catalog")
            catalog_version_replacements = replace_numeric_token(
                CATALOG_VERSION_FILES,
                old_catalog_version,
                new_catalog_version,
                "curated catalog version",
            )
            verifier_replacements = replace_paths(
                VERIFIER_VERSION_FILES,
                old_verifier,
                new_verifier,
                "verifier version",
                required=False,
            )
            contract_replacement_count = 0
            for old, new, label in token_changes:
                paths = operational_contract_paths(old)
                contract_replacement_count += replace_paths(
                    paths, old, new, label
                )
            if old_verifier != new_verifier:
                old_verifier_bytes = old_verifier.encode("ascii")
                new_verifier_bytes = new_verifier.encode("ascii")
                for path in VERIFIER_VERSION_FILES:
                    content = path.read_bytes()
                    if old_verifier_bytes in content:
                        raise RuntimeError(
                            "stale verifier version remains in {}".format(
                                path.relative_to(ROOT)
                            )
                        )
                    if new_verifier_bytes not in content:
                        raise RuntimeError(
                            "{} does not declare the adopted verifier version".format(
                                path.relative_to(ROOT)
                            )
                        )
            shutil.rmtree(str(PACK_ROOT))
            shutil.copytree(str(staged_packs), str(PACK_ROOT))

            imported = read_json(CATALOG_FILE)
            if imported.get("source_catalog_sha256") != new_catalog:
                raise RuntimeError("imported catalog does not match the core release set")
            if tracked_files_containing(old_core):
                raise RuntimeError("stale core commit pins remain after adoption")
            if old_catalog != new_catalog and tracked_files_containing(old_catalog):
                raise RuntimeError("stale catalog pins remain after adoption")
            for old, _new, label in token_changes:
                if operational_contract_paths(old):
                    raise RuntimeError(
                        "stale {} contract tokens remain after adoption".format(
                            label
                        )
                    )
            run(["git", "diff", "--check"])
            run([sys.executable, ROOT / "scripts" / "audit_system.py"])
        except Exception:
            restore_checkout(file_backups, pack_backup)
            raise

    print("core_adoption=ok")
    print("core={} -> {}".format(old_core, new_core))
    print("catalog={} -> {}".format(old_catalog, new_catalog))
    print("verifier={} -> {}".format(old_verifier, new_verifier))
    print("updated_pins={}/{}/{}/{}".format(
        core_replacements, catalog_replacements,
        catalog_version_replacements, verifier_replacements
    ))
    print("updated_contract_tokens={}".format(contract_replacement_count))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print("adopt_core_release: {}".format(error), file=sys.stderr)
        raise SystemExit(1)
