#!/usr/bin/env python3
"""Validate v3 challenges and build the generator-free verification kit.

The script intentionally imports only the separately built ``synsigra``
verifier wheel. Generation remains in the C++ worker.
"""

import argparse
import copy
import hashlib
import importlib.metadata
import json
import os
import pathlib
import shutil
import sys
import tempfile
import zipfile

import synsigra


KIT_CONTRACT = "synsigra_verification_kit_v3"
CORE_COMMIT = "65d995dcb1aea716bd77813001ace30d5a798b1c"
MEASUREMENT_COLUMNS = [
    "name", "value", "unit", "status", "scope", "time_seconds",
    "beat_index", "window_start_seconds", "window_end_seconds", "channel",
    "formula", "method_id", "preprocessing_policy_id", "confidence",
]


def _verifier_version():
    try:
        return importlib.metadata.version("synsigra")
    except importlib.metadata.PackageNotFoundError as error:
        raise RuntimeError("the Synsigra verifier wheel metadata is unavailable") from error


VERIFIER_VERSION = _verifier_version()


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _safe_relative(value):
    if not isinstance(value, str) or not value or len(value) > 512:
        return False
    if value.startswith("/") or "\\" in value:
        return False
    return all(
        part not in ("", ".", "..") and ":" not in part
        and all(ord(character) >= 0x20 for character in part)
        for part in value.split("/")
    )


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return "sha256:" + digest.hexdigest()


def _reject_duplicate_pairs(pairs):
    result = {}
    for key, value in pairs:
        _require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def _load_json_file(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle, object_pairs_hook=_reject_duplicate_pairs)


def _role_paths(package):
    roles = {}
    for item in package.files:
        roles.setdefault(item["role"], []).append(item["path"])
    return dict((role, sorted(paths)) for role, paths in sorted(roles.items()))


def _single_role_path(package, role, required=True):
    paths = _role_paths(package).get(role, [])
    _require(
        len(paths) == 1 if required else len(paths) <= 1,
        "challenge role %s must occur %s" % (
            role, "exactly once" if required else "at most once",
        ),
    )
    return paths[0] if paths else None


def _external_noise_metadata(package):
    # Core intentionally classifies these case artifacts under the general
    # ground-truth role. Select them by their strict document contract instead
    # of guessing a filename that is not a manifest role.
    truth_paths = []
    documents = {}
    for path in _role_paths(package).get("ground_truth_metrics_json", []):
        document = _load_json_file(package.resolve(path))
        if document.get("contract") == "synsigra_external_noise_truth_v1":
            truth_paths.append(path)
            documents[path] = document
    assets = {}
    for path in truth_paths:
        document = documents[path]
        _require(
            isinstance(document, dict)
            and set(document) == {
                "schema_version", "contract", "release_allowed", "intervals",
            }
            and document.get("schema_version") == 1
            and document.get("contract") == "synsigra_external_noise_truth_v1"
            and document.get("release_allowed") is True
            and isinstance(document.get("intervals"), list)
            and document["intervals"],
            "external-noise truth is invalid or not releasable",
        )
        for interval in document["intervals"]:
            _require(isinstance(interval, dict), "external-noise interval is invalid")
            asset_id = interval.get("asset_id")
            digest = interval.get("content_sha256")
            redistribution = interval.get("redistribution")
            _require(
                isinstance(asset_id, str) and asset_id
                and isinstance(digest, str) and len(digest) == 71
                and digest.startswith("sha256:")
                and all(character in "0123456789abcdef" for character in digest[7:])
                and redistribution in ("rendered_output", "source_and_output")
                and isinstance(interval.get("source_uri"), str)
                and isinstance(interval.get("license"), str),
                "external-noise asset identity is invalid or local-only",
            )
            identity = {
                "asset_id": asset_id,
                "content_sha256": digest,
                "redistribution": redistribution,
                "source_uri": interval["source_uri"],
                "license": interval["license"],
            }
            previous = assets.get(asset_id)
            _require(
                previous is None or previous == identity,
                "external-noise asset identity is inconsistent",
            )
            assets[asset_id] = identity
    return {
        "used": bool(truth_paths),
        "release_allowed": True,
        "assets": [assets[key] for key in sorted(assets)],
        "truth_paths": truth_paths,
    }


def _validate_documents(package):
    _require(
        package.manifest.get("schema_version") == 1
        and package.manifest.get("contract") == "synsigra_challenge_package_v3",
        "challenge package contract is unsupported",
    )
    _single_role_path(package, "scoring_manifest_json")
    _single_role_path(package, "submission_manifest_json")
    _single_role_path(package, "submission_formats_json")
    scoring = package.scoring_manifest()
    submission = package.submission_manifest()
    formats = package.submission_formats()
    _require(
        scoring.get("schema_version") == 3
        and scoring.get("contract") == "synsigra_scoring_manifest_v3",
        "scoring manifest schema is unsupported",
    )
    _require(
        scoring.get("scoring_manifest_contract_version") ==
        "synsigra_scoring_manifest_v3",
        "scoring manifest contract is unsupported",
    )
    _require(
        scoring.get("submission_contract_version") == "synsigra_submission_v1",
        "scoring manifest submission contract is unsupported",
    )
    _require(scoring.get("package_id") == package.package_id, "scoring package ID mismatch")
    _require(scoring.get("pack_version") == package.version, "scoring pack version mismatch")
    _require(scoring.get("generator_version") == "0.10.0-dev", "generator version mismatch")
    _require(scoring.get("generator_git_commit") == CORE_COMMIT, "generator commit mismatch")
    _require(
        isinstance(submission, dict)
        and set(submission) == {"schema_version", "contract", "challenge", "algorithm", "outputs"}
        and submission.get("schema_version") == 1
        and submission.get("contract") == "synsigra_submission_v1",
        "submission template contract is unsupported",
    )
    expected_identity = {
        "package_id": scoring.get("package_id"),
        "pack_version": scoring.get("pack_version"),
        "pack_fingerprint": scoring.get("pack_fingerprint"),
    }
    _require(submission.get("challenge") == expected_identity, "submission challenge identity mismatch")
    _require(
        isinstance(formats, dict)
        and formats.get("schema_version") == 2
        and formats.get("contract") == "synsigra_submission_formats_v2"
        and isinstance(formats.get("formats"), list),
        "submission format contract is unsupported",
    )
    format_names = [item.get("name") for item in formats["formats"] if isinstance(item, dict)]
    _require(
        format_names == [
            "point_events_json_v1", "point_events_csv_v1",
            "interval_events_json_v1", "interval_events_csv_v1",
            "measurement_values_json_v2", "measurement_values_csv_v2",
        ],
        "submission formats are not the exact v2 public set",
    )
    csv_format = formats["formats"][-1]
    _require(
        csv_format.get("columns") == MEASUREMENT_COLUMNS
        and csv_format.get("required_columns") == MEASUREMENT_COLUMNS,
        "measurement CSV is not the strict 14-column v2 adapter",
    )
    for item in scoring.get("targets", []):
        accepted = item.get("accepted_formats", [])
        _require(
            isinstance(accepted, list)
            and accepted
            and all(name in format_names for name in accepted),
            "target advertises a format outside the exact v2 public set",
        )
        if item.get("target") in {
            "rr_interval", "qtc", "hrv", "morphology_assertions",
            "ecg_ppg_alignment", "ppg_optical", "prv",
            "respiratory_rate", "rhythm_burden",
        }:
            _require(
                accepted == [
                    "measurement_values_json_v2", "measurement_values_csv_v2",
                ],
                "measurement target does not use the uniform v2 interface",
            )
    protocol_path = _single_role_path(
        package, "verification_protocol_json", required=False,
    )
    protocol = None
    if protocol_path is not None:
        protocol = package.verification_protocol()
        _require(
            scoring.get("verification_protocol_path") == protocol_path,
            "scoring manifest protocol path does not match its manifest role",
        )
    else:
        _require(
            scoring.get("verification_protocol_path") in (None, ""),
            "scoring manifest declares a missing verification protocol",
        )
    return scoring, submission, formats, protocol


def _supported_matrix(scoring):
    return set(
        (case.get("case_id"), item.get("target"))
        for case in scoring.get("cases", [])
        for item in case.get("scoring", [])
        if item.get("supported") is True
    )


def _verification_metadata(package, scoring, protocol):
    if protocol is None:
        return {
            "mode": "diagnostic",
            "evidence_eligible": False,
            "matrix_complete": None,
            "evidence_result": "not_run",
            "policy_result": "not_run",
            "notice": "No packaged protocol v2; local verification must be explicit diagnostic mode and cannot produce evidence.",
            "protocol": None,
        }
    identity = package.verification_protocol_identity()
    required = set(
        (item["case_id"], target)
        for item in protocol["required_case_targets"]
        for target in item["targets"]
    )
    matrix_complete = required == _supported_matrix(scoring)
    _require(matrix_complete, "verification protocol matrix does not equal scoreable matrix")
    verdict_scope = protocol.get("verdict_scope", "aggregate")
    _require(
        verdict_scope in ("aggregate", "per_case"),
        "verification protocol verdict scope is unsupported",
    )
    acceptance_profile_id = (
        "per_case_profiles"
        if verdict_scope == "per_case"
        else protocol["acceptance_profile"]["profile_id"]
    )
    identity.update({
        "context_of_use": protocol["context_of_use"],
        "scoring_contract": protocol["scoring_contract"],
        "verdict_scope": verdict_scope,
        "acceptance_profile_id": acceptance_profile_id,
        "required_case_target_count": len(required),
        "evidence_boundary": protocol["evidence_boundary"],
    })
    return {
        "mode": "evidence",
        "evidence_eligible": True,
        "matrix_complete": True,
        "evidence_result": "not_run",
        "policy_result": "not_run",
        "notice": "Evidence outcome is produced only after the customer runs the packaged protocol without overrides.",
        "protocol": identity,
    }


def _submission_sources(package, submission):
    manifest_path = _single_role_path(package, "submission_manifest_json")
    formats_path = _single_role_path(package, "submission_formats_json")
    root = pathlib.PurePosixPath(manifest_path).parent
    _require(
        pathlib.PurePosixPath(formats_path).parent == root,
        "submission manifest and formats must share a directory",
    )
    listed = set(item["path"] for item in package.files)
    sources = [(manifest_path, "submission.json"), (formats_path, "formats.json")]
    seen = {"submission.json", "formats.json"}
    outputs = submission.get("outputs")
    _require(isinstance(outputs, list), "submission outputs must be an array")
    for index, output in enumerate(outputs):
        _require(
            isinstance(output, dict)
            and set(output) == {"case_id", "target", "format", "path"}
            and _safe_relative(output.get("path")),
            "submission output %d is invalid" % index,
        )
        destination = output["path"]
        source = str(root / pathlib.PurePosixPath(destination))
        _require(source in listed, "submission template output is not manifest-listed: " + destination)
        _require(destination not in seen, "duplicate submission output path: " + destination)
        seen.add(destination)
        sources.append((source, destination))
    return sources


def _validate_submission_template(package, scoring, submission, sources):
    with tempfile.TemporaryDirectory(prefix="synsigra_submission_") as directory:
        for source, destination in sources:
            target = os.path.join(directory, *destination.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(package.resolve(source), target)
        validation = copy.deepcopy(submission)
        validation["algorithm"] = {"name": "kit-validation", "version": "1"}
        with open(os.path.join(directory, "submission.json"), "w", encoding="utf-8") as handle:
            json.dump(validation, handle, separators=(",", ":"), sort_keys=True)
        synsigra.load_submission(directory, package, scoring)


def inspect_challenge(path):
    _require(_verifier_version() == VERIFIER_VERSION, "unexpected Synsigra verifier version")
    with synsigra.load_challenge(path) as package:
        integrity = package.verify_integrity()
        scoring, submission, formats, protocol = _validate_documents(package)
        sources = _submission_sources(package, submission)
        _validate_submission_template(package, scoring, submission, sources)
        targets = []
        for item in scoring.get("targets", []):
            targets.append(dict(
                (key, item.get(key))
                for key in (
                    "target", "supported", "score_type", "accepted_formats",
                    "recommended_format", "cases",
                )
            ))
        cases = []
        for item in scoring.get("cases", []):
            cases.append({
                "case_id": item.get("case_id"),
                "scenario_id": item.get("scenario_id"),
                "targets": item.get("targets", []),
                "outputs": [
                    dict((key, score.get(key)) for key in (
                        "target", "supported", "score_type", "accepted_formats",
                        "recommended_format", "recommended_path",
                    ))
                    for score in item.get("scoring", [])
                ],
            })
        return {
            "schema_version": 1,
            "contract": "synsigra_saas_challenge_metadata_v1",
            "verifier_version": VERIFIER_VERSION,
            "challenge_contract": package.manifest.get("contract"),
            "package_id": package.package_id,
            "name": package.name,
            "pack_version": package.version,
            "pack_fingerprint": scoring.get("pack_fingerprint"),
            "generator_version": scoring.get("generator_version"),
            "generator_git_commit": scoring.get("generator_git_commit"),
            "scoring_manifest_contract": scoring.get("scoring_manifest_contract_version"),
            "submission_contract": submission.get("contract"),
            "submission_formats_contract": formats.get("contract"),
            "measurement_values_contract": "synsigra_measurement_values_v2",
            "measurement_truth_contract": "synsigra_measurement_truth_v2",
            "measurement_scoring_contract": "synsigra_measurement_score_v2",
            "local_verification_contract": "synsigra_local_verification_v3",
            "case_count": len(package.cases),
            "targets": targets,
            "cases": cases,
            "submission_outputs": submission.get("outputs", []),
            "submission_formats": formats,
            "verification_protocol": protocol,
            "verification": _verification_metadata(package, scoring, protocol),
            "external_noise": _external_noise_metadata(package),
            "roles": _role_paths(package),
            "integrity": integrity,
        }


def _write_file_to_zip(archive, source, destination):
    info = zipfile.ZipInfo(destination, date_time=(2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100640 << 16
    with open(source, "rb") as handle, archive.open(info, "w") as target:
        shutil.copyfileobj(handle, target, length=1024 * 1024)


def _write_text_to_zip(archive, destination, text):
    info = zipfile.ZipInfo(destination, date_time=(2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100640 << 16
    archive.writestr(info, text.encode("utf-8"))


def _kit_readme(metadata):
    evidence = metadata["verification"]["evidence_eligible"]
    per_case = (
        evidence
        and metadata["verification"]["protocol"].get("verdict_scope") == "per_case"
    )
    command = (
        "synsigra-verify challenge submission verification-results --force"
        if evidence else
        "synsigra-verify challenge submission verification-results --mode diagnostic --force"
    )
    if per_case:
        mode_text = (
            "This package contains an immutable per-case protocol v2. The command "
            "evaluates every complete signal independently; cases are not split, "
            "pooled, averaged, or allowed to compensate for one another. Do not add "
            "a profile, case, or target override to an evidence run."
        )
    elif evidence:
        mode_text = (
            "This package contains an immutable protocol v2. The command runs the "
            "complete package-authoritative evidence matrix and embedded numeric "
            "acceptance policy. Do not add a profile, case, or target override to "
            "an evidence run."
        )
    else:
        mode_text = (
            "This package has no pre-specified protocol v2. Verification is therefore "
            "explicitly diagnostic and is never evidence-eligible."
        )
    return """Synsigra generator-free verification kit

1. Install the Synsigra verifier wheel downloaded from the product UI:
   python -m pip install synsigra-{version}-py3-none-any.whl
2. Replace the placeholder algorithm name/version and example output rows in
   submission/. Keep submission.json paths, target names and formats unchanged.
3. Run from this verification-kit directory:
   {command}
4. Open verification-results/index.html. It links every case-target detail
   page. verification-results/evidence.json is the single canonical
   machine-readable evidence record.

{mode_text}

RR, QT/QTc, HRV, morphology, alignment, optical, PRV, respiration and rhythm
burden outputs all use measurement_values_json_v2 (recommended) or the strict
14-column measurement_values_csv_v2 adapter. Window bounds, method ID and
preprocessing-policy ID are part of measurement identity and must be preserved.

The challenge/ directory is the immutable, integrity-protected challenge and
contains its manifest, provenance and engineering claim boundary.
The submission/ directory is a byte-preserved working template selected by
manifest roles, not filename assumptions. Keep the kit, your algorithm build
identity/configuration and verification-results together as engineering evidence.

Synthetic engineering QA evidence; not diagnosis, nor clinical evidence.
""".format(command=command, mode_text=mode_text, version=VERIFIER_VERSION)


def build_kit(source, destination):
    metadata = inspect_challenge(source)
    destination = os.path.abspath(destination)
    parent = os.path.dirname(destination)
    os.makedirs(parent, exist_ok=True)
    temporary = destination + ".tmp.%d" % os.getpid()
    try:
        with synsigra.load_challenge(source) as package:
            package.verify_integrity()
            scoring, submission, _, _ = _validate_documents(package)
            sources = _submission_sources(package, submission)
            _validate_submission_template(package, scoring, submission, sources)
            with zipfile.ZipFile(temporary, "w", allowZip64=True) as archive:
                manifest = os.path.join(package.root, "manifest.json")
                _write_file_to_zip(
                    archive, manifest,
                    "verification-kit/challenge/manifest.json",
                )
                for item in sorted(package.files, key=lambda value: value["path"]):
                    _write_file_to_zip(
                        archive,
                        package.resolve(item["path"]),
                        "verification-kit/challenge/" + item["path"],
                    )
                for source_path, target_path in sources:
                    _write_file_to_zip(
                        archive,
                        package.resolve(source_path),
                        "verification-kit/submission/" + target_path,
                    )
                _write_text_to_zip(
                    archive,
                    "verification-kit/README.txt",
                    _kit_readme(metadata),
                )
        with zipfile.ZipFile(temporary, "r") as archive:
            _require(archive.testzip() is None, "verification kit archive is corrupt")
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    result = dict(metadata)
    result.update({
        "kit_contract": KIT_CONTRACT,
        "kit_sha256": _sha256(destination),
        "kit_size_bytes": os.path.getsize(destination),
    })
    return result


def main(argv=None):
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    inspect_parser = subparsers.add_parser("inspect")
    inspect_parser.add_argument("challenge")
    kit_parser = subparsers.add_parser("build-kit")
    kit_parser.add_argument("challenge")
    kit_parser.add_argument("output")
    args = parser.parse_args(argv)
    result = (
        inspect_challenge(args.challenge)
        if args.command == "inspect"
        else build_kit(args.challenge, args.output)
    )
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print("challenge artifact validation failed: %s" % error, file=sys.stderr)
        sys.exit(1)
