#!/usr/bin/env python3
"""Audit the complete core-to-SaaS curated release without changing it.

The quick mode is suitable for every local/release build.  Full mode also
renders every curated pack through the pinned generator and audits its truth
and report tree.  All executable paths are fixed repository/runtime paths so
the script is safe to expose through the stable approval wrapper.
"""

from __future__ import annotations

import argparse
import ast
import configparser
import hashlib
import json
import math
import pathlib
import re
import shutil
import subprocess
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE = ROOT.parent / "signal_synth"
PACK_ROOT = ROOT / "packs"
CORE_CATALOG = CORE / "examples" / "catalog" / "verification_packs_v1.json"
CORE_METADATA = CORE / "examples" / "catalog" / "curated_pack_metadata_v1.json"
SAAS_METADATA = PACK_ROOT / "curated_pack_metadata_v1.catalog"
PROFILE_IDS = ("level_1", "level_2", "level_3")
PROFILE_RANKS = (1, 2, 3)
SHA256_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
PIN_RE = re.compile(
    r'SYN_SIG_RA_EXPECTED_SIGNAL_SYNTH_COMMIT\s+"([0-9a-f]{40})"'
)
REMOVED_CATALOG_FIELDS = {
    "recommended_profile",
    "supported_threshold_profiles",
    "threshold_profile_contract",
}


class AuditError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AuditError(message)


def read_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise AuditError(f"{path}: invalid JSON: {error}") from error
    require(isinstance(value, dict), f"{path}: root must be an object")
    return value


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, separators=(",", ":"), sort_keys=True).encode("utf-8")


def canonical_hash(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_bytes(value)).hexdigest()


def command(
    values: list[str | pathlib.Path],
    cwd: pathlib.Path = ROOT,
) -> str:
    completed = subprocess.run(
        [str(value) for value in values],
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode:
        tail = "\n".join(completed.stdout.splitlines()[-80:])
        raise AuditError(
            "{} failed ({}):\n{}".format(
                " ".join(str(value) for value in values),
                completed.returncode,
                tail,
            ).rstrip()
        )
    return completed.stdout


def git_files(root: pathlib.Path) -> list[pathlib.Path]:
    output = command(["git", "ls-files", "-z"], root)
    return [
        root / item
        for item in output.split("\0")
        if item
    ]


def safe_resolve(path: pathlib.Path, parent: pathlib.Path, label: str) -> pathlib.Path:
    resolved = path.resolve()
    try:
        resolved.relative_to(parent.resolve())
    except ValueError as error:
        raise AuditError(f"{label} escapes {parent}") from error
    require(resolved.is_file(), f"{label} is not a regular file: {resolved}")
    require(not path.is_symlink(), f"{label} must not be a symlink: {path}")
    return resolved


def core_pin() -> str:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = PIN_RE.search(cmake)
    require(match is not None, "CMakeLists.txt has no full signal_synth commit pin")
    return match.group(1)


def core_verifier_version() -> str:
    parser = configparser.ConfigParser()
    parser.read(str(CORE / "setup.cfg"), encoding="utf-8")
    version = parser.get("metadata", "version", fallback="")
    require(bool(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version)),
            "signal_synth setup.cfg has no numeric verifier version")
    return version


def threshold_limits(document: dict[str, Any]) -> dict[tuple[str, ...], float]:
    limits: dict[tuple[str, ...], float] = {}

    def visit(value: Any, path: tuple[str, ...]) -> None:
        if not isinstance(value, dict):
            return
        for key, child in value.items():
            next_path = path + (key,)
            if key in ("min", "max"):
                require(
                    isinstance(child, (int, float))
                    and not isinstance(child, bool)
                    and math.isfinite(child),
                    "non-finite evidence threshold at " + ".".join(next_path),
                )
                limits[next_path] = float(child)
            else:
                visit(child, next_path)

    if "acceptance_profile" in document:
        visit(document["acceptance_profile"].get("targets"), ("aggregate",))
    for stratum in document.get("acceptance_strata", []):
        visit(
            stratum.get("acceptance_profile", {}).get("targets"),
            ("stratum", stratum.get("id", "")),
        )
    return limits


def audit_profile_envelope(pack: dict[str, Any], metadata: dict[str, Any]) -> int:
    pack_id = metadata["pack_id"]
    envelope = metadata.get("evidence_profiles")
    require(isinstance(envelope, dict), f"{pack_id}: missing evidence_profiles")
    protocol_path = pack.get("verification_protocol_path")
    profiles = envelope.get("profiles")
    require(isinstance(profiles, list), f"{pack_id}: profiles must be an array")

    if not protocol_path:
        require(envelope.get("available") is False,
                f"{pack_id}: diagnostic pack says evidence is available")
        require(envelope.get("default_profile_id") == "",
                f"{pack_id}: diagnostic pack has a default evidence profile")
        require(not profiles, f"{pack_id}: diagnostic pack has evidence profiles")
        return 0

    require(envelope.get("available") is True,
            f"{pack_id}: evidence pack says evidence is unavailable")
    require(envelope.get("default_profile_id") == "level_2",
            f"{pack_id}: Level 2 must be the default")
    require(envelope.get("policy_contract") == "synsigra_evidence_profile_policy_v1",
            f"{pack_id}: evidence profile policy mismatch")
    require(envelope.get("protocol_contract") == "synsigra_verification_protocol_v3",
            f"{pack_id}: verification protocol mismatch")
    require(tuple(item.get("profile_id") for item in profiles) == PROFILE_IDS,
            f"{pack_id}: evidence profile IDs/order mismatch")
    require(tuple(item.get("rank") for item in profiles) == PROFILE_RANKS,
            f"{pack_id}: evidence profile ranks mismatch")

    documents = [item.get("document") for item in profiles]
    require(all(isinstance(item, dict) for item in documents),
            f"{pack_id}: evidence profile document is missing")
    stable_fields = (
        "schema_version",
        "contract",
        "pack_id",
        "context_of_use",
        "scoring_contract",
        "required_case_targets",
        "stress_strata",
        "truth_policy",
        "evidence_boundary",
        "verdict_scope",
    )
    baseline = documents[1]
    for item, document in zip(profiles, documents):
        profile_id = item["profile_id"]
        require(document.get("pack_id") == pack_id,
                f"{pack_id}/{profile_id}: protocol pack ID mismatch")
        require(document.get("protocol_id") == item.get("protocol_id"),
                f"{pack_id}/{profile_id}: protocol ID mismatch")
        require(document.get("evidence_profile", {}).get("profile_id") == profile_id,
                f"{pack_id}/{profile_id}: embedded profile mismatch")
        require(item.get("protocol_sha256") == canonical_hash(document),
                f"{pack_id}/{profile_id}: protocol SHA-256 mismatch")
        require(SHA256_RE.fullmatch(item.get("protocol_sha256", "")) is not None,
                f"{pack_id}/{profile_id}: malformed protocol SHA-256")
        for field in stable_fields:
            require(document.get(field) == baseline.get(field),
                    f"{pack_id}/{profile_id}: {field} changes between levels")

    level_limits = [threshold_limits(document) for document in documents]
    require(level_limits[0], f"{pack_id}: evidence protocol has no numeric gates")
    require(
        set(level_limits[0]) == set(level_limits[1]) == set(level_limits[2]),
        f"{pack_id}: evidence levels do not contain the same gates",
    )
    epsilon = 1e-12
    for path in level_limits[1]:
        low, middle, high = (limits[path] for limits in level_limits)
        operator = path[-1]
        if operator == "min":
            require(low <= middle + epsilon and middle <= high + epsilon,
                    f"{pack_id}: non-monotonic minimum gate {'.'.join(path)}")
        else:
            require(low + epsilon >= middle and middle + epsilon >= high,
                    f"{pack_id}: non-monotonic maximum gate {'.'.join(path)}")
    return len(profiles)


def transformed_pack_and_files(
    source_pack: pathlib.Path,
    metadata: dict[str, Any],
    expected_files: set[pathlib.Path],
) -> dict[str, Any]:
    pack = read_json(source_pack)
    pack_id = metadata["pack_id"]
    require(pack.get("pack_id") == pack_id, f"{pack_id}: source pack ID mismatch")
    require(metadata.get("source", {}).get("source_content_sha256") == canonical_hash(pack),
            f"{pack_id}: source content SHA-256 mismatch")

    scenario_root = CORE / "examples" / "scenarios"
    for scenario in pack.get("scenarios", []):
        relative = scenario.get("path")
        require(isinstance(relative, str) and relative,
                f"{pack_id}: scenario path is missing")
        source = safe_resolve(
            source_pack.parent / relative,
            scenario_root,
            f"{pack_id} scenario",
        )
        destination = PACK_ROOT / "scenarios" / source.relative_to(
            scenario_root.resolve()
        )
        require(destination.is_file(), f"{pack_id}: imported scenario is missing")
        require(not destination.is_symlink(), f"{pack_id}: imported scenario is a symlink")
        require(source.read_bytes() == destination.read_bytes(),
                f"{pack_id}: imported scenario differs: {destination}")
        expected_files.add(destination)
        scenario["path"] = destination.relative_to(PACK_ROOT).as_posix()

    profile_count = audit_profile_envelope(pack, metadata)
    envelope = metadata["evidence_profiles"]
    if profile_count:
        for profile in envelope["profiles"]:
            filename = f"{pack_id}__{profile['profile_id']}_expectations.json"
            destination = PACK_ROOT / filename
            require(destination.is_file(), f"{pack_id}: missing {filename}")
            require(not destination.is_symlink(), f"{pack_id}: {filename} is a symlink")
            require(destination.read_bytes() == canonical_bytes(profile["document"]),
                    f"{pack_id}: {filename} is not the catalog document")
            expected_files.add(destination)
            if profile["profile_id"] == envelope["default_profile_id"]:
                pack["verification_protocol_path"] = filename

    actual_pack = PACK_ROOT / f"{pack_id}.json"
    require(read_json(actual_pack) == pack, f"{pack_id}: imported pack differs from core")
    expected_files.add(actual_pack)
    return pack


def audit_catalog() -> tuple[int, int, int]:
    require(CORE.is_dir(), "sibling ../signal_synth checkout is missing")
    pin = core_pin()
    head = command(["git", "rev-parse", "HEAD"], CORE).strip()
    require(head == pin, f"core HEAD {head} differs from SaaS pin {pin}")

    source_catalog = read_json(CORE_CATALOG)
    core_metadata = read_json(CORE_METADATA)
    saas_metadata = read_json(SAAS_METADATA)
    require(core_metadata == saas_metadata,
            "SaaS catalog is not the current core curated release")
    require(source_catalog.get("version") == saas_metadata.get("catalog_version"),
            "source/exported catalog versions differ")
    require(
        saas_metadata.get("pack_count") == len(saas_metadata.get("packs", [])),
        "catalog pack_count differs from packs[]",
    )
    require(
        saas_metadata.get("source_catalog_sha256") == canonical_hash(source_catalog),
        "catalog source SHA-256 does not match the source catalog",
    )
    verifier = core_verifier_version()
    entries = saas_metadata["packs"]
    pack_ids = [item.get("pack_id") for item in entries]
    require(all(isinstance(item, str) and item for item in pack_ids),
            "catalog contains an invalid pack ID")
    require(len(pack_ids) == len(set(pack_ids)), "catalog contains duplicate pack IDs")

    expected_files = {SAAS_METADATA}
    evidence_profiles = 0
    case_count = 0
    source_ids = {
        item.get("pack_id"): item for item in source_catalog.get("packs", [])
    }
    require(set(source_ids) == set(pack_ids),
            "source and exported catalog pack IDs differ")
    for metadata in entries:
        pack_id = metadata["pack_id"]
        require(not (REMOVED_CATALOG_FIELDS & set(metadata)),
                f"{pack_id}: obsolete threshold-profile discovery fields remain")
        require(not (REMOVED_CATALOG_FIELDS & set(source_ids[pack_id])),
                f"{pack_id}: obsolete threshold-profile source fields remain")
        compatibility = metadata.get("generator_compatibility", {})
        require(compatibility.get("local_verifier_min_version") == verifier,
                f"{pack_id}: verifier version drift")
        require(compatibility.get("verification_protocol_contract")
                == "synsigra_verification_protocol_v3",
                f"{pack_id}: verification protocol contract drift")

        source = metadata.get("source", {})
        catalog_path = safe_resolve(
            CORE / source.get("catalog_path", ""),
            CORE / "examples",
            f"{pack_id} source catalog",
        )
        require(catalog_path == CORE_CATALOG.resolve(),
                f"{pack_id}: unexpected source catalog")
        source_pack = safe_resolve(
            catalog_path.parent / source.get("pack_path", ""),
            CORE / "examples" / "packs",
            f"{pack_id} source pack",
        )
        pack = transformed_pack_and_files(source_pack, metadata, expected_files)
        profiles = metadata["evidence_profiles"]["profiles"]
        evidence_profiles += len(profiles)
        ids = [case.get("id") for case in pack.get("scenarios", [])]
        require(ids == metadata.get("case_ids"),
                f"{pack_id}: pack and metadata case order differ")
        require(len(ids) == metadata.get("case_count") == len(metadata.get("cases", [])),
                f"{pack_id}: inconsistent case count")
        require(len(ids) == len(set(ids)), f"{pack_id}: duplicate case IDs")
        case_count += len(ids)

    noise_source = CORE / "examples" / "assets" / "noise"
    for source in sorted(path for path in noise_source.rglob("*") if path.is_file()):
        require(not source.is_symlink(), f"noise asset is a symlink: {source}")
        destination = PACK_ROOT / "noise_assets" / source.relative_to(noise_source)
        require(destination.is_file(), f"imported noise asset is missing: {destination}")
        require(not destination.is_symlink(), f"imported noise asset is a symlink")
        require(source.read_bytes() == destination.read_bytes(),
                f"imported noise asset differs: {destination}")
        expected_files.add(destination)

    actual_files = {
        path for path in PACK_ROOT.rglob("*") if path.is_file() or path.is_symlink()
    }
    extra = sorted(path.relative_to(ROOT).as_posix() for path in actual_files - expected_files)
    missing = sorted(path.relative_to(ROOT).as_posix() for path in expected_files - actual_files)
    require(not extra, "untracked/obsolete imported pack files: " + ", ".join(extra))
    require(not missing, "missing imported pack files: " + ", ".join(missing))
    return len(entries), case_count, evidence_profiles


def audit_current_documents() -> None:
    contract_source = (
        ROOT / "src" / "integration" / "core_contract.cpp"
    ).read_text(encoding="utf-8")
    integration_match = re.search(
        r'kIntegration\[\]\s*=\s*"(synsigra_core_integration_v([0-9]+))"',
        contract_source,
    )
    require(integration_match is not None,
            "unable to read the SaaS integration contract")
    integration_contract, integration_version = integration_match.groups()
    catalog = read_json(SAAS_METADATA)
    protocols = {
        item.get("generator_compatibility", {}).get(
            "verification_protocol_contract"
        )
        for item in catalog["packs"]
    }
    require(len(protocols) == 1 and None not in protocols,
            "catalog packs disagree on the verification protocol contract")
    protocol_contract = protocols.pop()
    protocol_version = protocol_contract.rsplit("_v", 1)[-1]
    current_documents = (
        ROOT / "README.md",
        ROOT / "doc" / "API_GUIDE.md",
        ROOT / "doc" / "CODEX_API_CLIENT_GUIDE.md",
        ROOT / "doc" / "DEVELOPER_REFERENCE.md",
        ROOT / "doc" / "MCP_SERVER.md",
        ROOT / "doc" / "PACK_CATALOG.md",
        ROOT / "doc" / "PRODUCT_CAPABILITIES.md",
        ROOT / "doc" / "openapi.yaml",
    )
    for path in current_documents:
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"synsigra_core_integration_v([0-9]+)", text):
            require(
                match.group(0) == integration_contract,
                f"{path}: stale integration contract {match.group(0)}",
            )
        for match in re.finditer(r"\bcore v([0-9]+)\b", text, re.IGNORECASE):
            require(
                match.group(1) == integration_version,
                f"{path}: stale core v{match.group(1)} label",
            )
        for match in re.finditer(
            r"synsigra_verification_protocol_v([0-9]+)", text
        ):
            require(
                match.group(1) == protocol_version,
                f"{path}: stale verification protocol {match.group(0)}",
            )
        require(
            not (REMOVED_CATALOG_FIELDS & set(
                re.findall(
                    r"\b(?:recommended_profile|supported_threshold_profiles|threshold_profile_contract)\b",
                    text,
                )
            )) or path == ROOT / "doc" / "DEVELOPER_REFERENCE.md",
            f"{path}: removed catalog profile fields remain documented",
        )


def audit_script_syntax() -> int:
    checked = 0
    for root in (ROOT, CORE):
        for path in git_files(root):
            if not path.is_file():
                continue
            if path.suffix == ".py":
                try:
                    ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
                except (OSError, SyntaxError) as error:
                    raise AuditError(f"{path}: Python syntax error: {error}") from error
                checked += 1
            elif path.suffix == ".sh":
                first = path.read_text(encoding="utf-8").splitlines()[:1]
                shell = "bash" if first and "bash" in first[0] else "sh"
                command([shell, "-n", path])
                checked += 1
    node = shutil.which("node") or shutil.which("nodejs")
    if node:
        for path in sorted((ROOT / "web").rglob("*.js")):
            command([node, "--check", path])
            checked += 1
    return checked


def pinned_cli(pin: str) -> pathlib.Path:
    candidates = (
        ROOT / "build" / "signal_synth_live" / "signal-synth",
        ROOT / "build" / "e2e" / "signal_synth_cli" / "signal-synth",
        CORE / "build" / "signal-synth",
        pathlib.Path("/opt/signal_synth/bin/signal-synth"),
    )
    for candidate in candidates:
        if not candidate.is_file() or candidate.is_symlink():
            continue
        try:
            contract = json.loads(command([candidate, "contract"]))
        except (AuditError, ValueError):
            continue
        if contract.get("generator", {}).get("git_commit") == pin:
            return candidate
    raise AuditError(
        "no fixed-path signal-synth CLI matches the pinned core; "
        "run scripts/task2_release_dolgok.py core-refresh"
    )


def audit_full_generation(pack_count: int) -> str:
    pin = core_pin()
    cli = pinned_cli(pin)
    for pack_path in sorted(PACK_ROOT.glob("*.json")):
        if pack_path.name.endswith("_expectations.json"):
            continue
        command([cli, "pack", "validate", pack_path])
    output = command([
        sys.executable,
        CORE / "scripts" / "audit_curated_truth.py",
        "--cli",
        cli,
    ])
    summary = next(
        (line for line in reversed(output.splitlines()) if line.startswith("truth_audit=")),
        "",
    )
    require(summary.startswith(f"truth_audit=ok packs={pack_count} "),
            "truth audit did not cover the complete catalog")
    return summary


def run() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--full",
        action="store_true",
        help="Also render and audit all curated challenge packages.",
    )
    args = parser.parse_args()

    pack_count, case_count, profile_count = audit_catalog()
    audit_current_documents()
    command([sys.executable, CORE / "scripts" / "materialize_evidence_protocols.py"])
    script_count = audit_script_syntax()
    truth = audit_full_generation(pack_count) if args.full else ""
    print(
        "system_audit=ok mode={} packs={} cases={} evidence_profiles={} scripts={}".format(
            "full" if args.full else "quick",
            pack_count,
            case_count,
            profile_count,
            script_count,
        )
    )
    if truth:
        print(truth)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (AuditError, OSError, KeyError, ValueError) as error:
        print(f"system_audit=failed error={error}", file=sys.stderr)
        raise SystemExit(1)
