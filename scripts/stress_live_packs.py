#!/usr/bin/env python3
"""Exercise all live Synsigra packs end-to-end.

The script is intentionally dependency-free so it can run on the VPS without
creating a Python environment. It queues each pack as a live job, waits for a
terminal state, validates the generated manifest/package ZIP, and validates
the same role-driven verification kit that customers download.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from typing import Any


def read_api_key(explicit: str | None, key_file: str) -> str:
    if explicit:
        return explicit.strip()
    from_env = os.environ.get("SYN_SIG_RA_API_KEY")
    if from_env:
        return from_env.strip()
    try:
        return pathlib.Path(key_file).read_text(encoding="utf-8").strip()
    except PermissionError:
        pass
    except FileNotFoundError:
        pass

    completed = subprocess.run(
        ["sudo", "cat", key_file],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode == 0 and completed.stdout.strip():
        return completed.stdout.strip()
    raise SystemExit(
        "No API key available. Set SYN_SIG_RA_API_KEY, pass --api-key, "
        f"or make {key_file} readable."
    )


class Client:
    def __init__(self, base_url: str, api_key: str, timeout: int) -> None:
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.timeout = timeout

    def request_json(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
        auth: bool = True,
    ) -> dict[str, Any]:
        data = None
        headers = {"Accept": "application/json"}
        if auth:
            headers["Authorization"] = f"Bearer {self.api_key}"
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(
            self.base_url + path,
            data=data,
            method=method,
            headers=headers,
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read()
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", "replace")
            raise RuntimeError(f"{method} {path} failed: HTTP {exc.code}: {raw}") from exc
        if not raw:
            return {}
        try:
            parsed = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"{method} {path} returned invalid JSON") from exc
        if not isinstance(parsed, dict):
            raise RuntimeError(f"{method} {path} returned non-object JSON")
        return parsed

    def download(self, path: str, destination: pathlib.Path, auth: bool = True) -> int:
        headers = {"Accept": "*/*"}
        if auth:
            headers["Authorization"] = f"Bearer {self.api_key}"
        request = urllib.request.Request(
            self.base_url + path,
            method="GET",
            headers=headers,
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                total = 0
                with destination.open("wb") as handle:
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        total += len(chunk)
                        handle.write(chunk)
                return total
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", "replace")
            raise RuntimeError(f"GET {path} failed: HTTP {exc.code}: {raw}") from exc


def choose_project(client: Client, explicit_project: str | None) -> str:
    if explicit_project:
        return explicit_project
    body = client.request_json("GET", "/v1/projects")
    projects = body.get("projects")
    if not isinstance(projects, list) or not projects:
        raise RuntimeError("No projects are available for the API key")
    project_id = projects[0].get("project_id")
    if not isinstance(project_id, str) or not project_id:
        raise RuntimeError("Project response did not contain project_id")
    return project_id


def list_packs(client: Client, selected: list[str]) -> list[dict[str, Any]]:
    body = client.request_json("GET", "/v1/packs", auth=False)
    packs = body.get("packs")
    if not isinstance(packs, list):
        raise RuntimeError("Pack list response did not contain packs[]")
    valid = [pack for pack in packs if isinstance(pack, dict)]
    for pack in valid:
        if pack.get("catalog_version") != "3.0":
            raise RuntimeError("pack list contains a non-3.0 catalog entry")
        if pack.get("catalog_source_sha256") != "sha256:3a8b53b43dbecdeb834ed3faf0fddb8a859464ff4b822caaaa31830f5a06c88f":
            raise RuntimeError("pack list contains an unexpected catalog hash")
        if pack.get("integration_contract") != "synsigra_core_integration_v7":
            raise RuntimeError("pack list contains a non-v7 entry")
    if not selected:
        if len(valid) != 18:
            raise RuntimeError(f"expected 18 catalog packs, found {len(valid)}")
        return valid
    by_id = {pack.get("pack_id"): pack for pack in valid}
    missing = [pack_id for pack_id in selected if pack_id not in by_id]
    if missing:
        raise RuntimeError("Unknown selected pack(s): " + ", ".join(missing))
    return [by_id[pack_id] for pack_id in selected]


def wait_for_job(
    client: Client,
    job_id: str,
    poll_seconds: float,
    timeout_seconds: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    last_status = None
    last_print_at = 0.0
    while True:
        job = client.request_json("GET", f"/v1/jobs/{job_id}")
        status = job.get("status")
        now = time.monotonic()
        if status != last_status or now - last_print_at >= 15:
            print(f"    {job_id}: {status}", flush=True)
            last_status = status
            last_print_at = now
        if status in ("succeeded", "failed", "cancelled"):
            return job
        if now >= deadline:
            raise RuntimeError(f"Job {job_id} timed out after {timeout_seconds:.0f}s")
        time.sleep(poll_seconds)


def validate_manifest(path: pathlib.Path) -> None:
    with path.open("r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    if not manifest.get("package_id"):
        raise RuntimeError("manifest has no package_id")
    if manifest.get("contract") != "synsigra_challenge_package_v3":
        raise RuntimeError("manifest is not challenge package v3")
    if manifest.get("package_type") not in ("challenge", "scenario_pack"):
        raise RuntimeError("manifest package_type is not recognized")
    cases = manifest.get("cases")
    if not isinstance(cases, list) or not cases:
        raise RuntimeError("manifest has no cases[]")


def validate_zip(path: pathlib.Path, required_members: list[str] | None = None) -> None:
    with zipfile.ZipFile(path) as archive:
        bad = archive.testzip()
        if bad is not None:
            raise RuntimeError(f"zip CRC failed for {bad}")
        names = set(archive.namelist())
    for member in required_members or []:
        if member not in names:
            raise RuntimeError(f"zip is missing {member}")


def validate_job(job: dict[str, Any], pack: dict[str, Any]) -> None:
    if job.get("integration_contract") != "synsigra_core_integration_v7":
        raise RuntimeError("job has the wrong integration contract")
    if job.get("generator_git_commit") != "2531c5c21a1917f9704fa9562d0a32ebacc821da":
        raise RuntimeError("job was not rendered by the pinned generator")
    if job.get("pack_version") != pack.get("version"):
        raise RuntimeError("job pack version differs from the selected catalog entry")
    catalog = job.get("catalog")
    if not isinstance(catalog, dict) or catalog.get("version") != "3.0":
        raise RuntimeError("job does not preserve catalog 3.0 identity")
    if catalog.get("source_sha256") != "sha256:3a8b53b43dbecdeb834ed3faf0fddb8a859464ff4b822caaaa31830f5a06c88f":
        raise RuntimeError("job has the wrong catalog hash")
    challenge = job.get("challenge")
    if not isinstance(challenge, dict):
        raise RuntimeError("job has no normalized challenge metadata")
    expected = {
        "verifier_version": "0.11.0",
        "challenge_contract": "synsigra_challenge_package_v3",
        "scoring_manifest_contract": "synsigra_scoring_manifest_v3",
        "submission_contract": "synsigra_submission_v1",
        "submission_formats_contract": "synsigra_submission_formats_v2",
        "measurement_values_contract": "synsigra_measurement_values_v2",
        "measurement_truth_contract": "synsigra_measurement_truth_v2",
        "measurement_scoring_contract": "synsigra_measurement_score_v2",
        "local_verification_contract": "synsigra_local_verification_v3",
    }
    for key, value in expected.items():
        if challenge.get(key) != value:
            raise RuntimeError(f"challenge {key} is not {value}")
    if challenge.get("integrity", {}).get("ok") is not True:
        raise RuntimeError("challenge integrity was not verified")
    if not isinstance(challenge.get("submission_outputs"), list):
        raise RuntimeError("challenge has no role-selected submission outputs")
    expected_targets = {
        item.get("target")
        for key in ("scoreable_targets", "reference_only_targets")
        for item in pack.get(key, [])
        if isinstance(item, dict)
    }
    actual_targets = {
        item.get("target")
        for item in challenge.get("targets", [])
        if isinstance(item, dict)
    }
    if actual_targets != expected_targets:
        raise RuntimeError("challenge target coverage differs from the catalog")
    protocol = pack.get("verification_protocol")
    if not isinstance(protocol, dict):
        raise RuntimeError("catalog pack has no verification protocol envelope")
    expected_protocol = protocol.get("document") if protocol.get("available") else None
    if challenge.get("verification_protocol") != expected_protocol:
        raise RuntimeError("challenge verification protocol differs from the catalog")
    verification = challenge.get("verification")
    if not isinstance(verification, dict):
        raise RuntimeError("challenge has no verification-mode metadata")
    if protocol.get("available"):
        if verification.get("mode") != "evidence" or \
                verification.get("evidence_eligible") is not True or \
                verification.get("matrix_complete") is not True or \
                not isinstance(verification.get("protocol"), dict):
            raise RuntimeError("protocol pack is not evidence-ready")
    elif verification.get("mode") != "diagnostic" or \
            verification.get("evidence_eligible") is not False or \
            verification.get("matrix_complete") is not None or \
            verification.get("protocol") is not None:
        raise RuntimeError("protocol-free pack is not explicitly diagnostic")


def validate_kit(path: pathlib.Path, job: dict[str, Any]) -> None:
    prefix = "verification-kit/"
    required = [
        prefix + "README.txt",
        prefix + "challenge/manifest.json",
        prefix + "challenge/ENGINEERING_CLAIM_BOUNDARY.txt",
        prefix + "submission/submission.json",
        prefix + "submission/formats.json",
    ]
    validate_zip(path, required)
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if any(name.endswith("package.zip") for name in names):
            raise RuntimeError("verification kit contains a nested package ZIP")
        if prefix + "ENGINEERING_CLAIM_BOUNDARY.txt" in names or \
                prefix + "challenge-metadata.json" in names:
            raise RuntimeError("verification kit contains redundant top-level metadata")
        manifest = json.loads(
            archive.read(prefix + "challenge/manifest.json").decode("utf-8")
        )
        if manifest.get("contract") != "synsigra_challenge_package_v3":
            raise RuntimeError("verification kit challenge contract mismatch")
        if manifest.get("package_id") != job.get("challenge", {}).get("package_id"):
            raise RuntimeError("verification kit package identity mismatch")


def delete_job(client: Client, job_id: str) -> None:
    try:
        client.request_json("DELETE", f"/v1/jobs/{job_id}")
    except RuntimeError as exc:
        print(f"    warning: could not delete {job_id}: {exc}", file=sys.stderr)


def run() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--base-url",
        default=os.environ.get(
            "SYN_SIG_RA_BASE_URL", "https://www.timeonion.com/syn_sig_ra"
        ),
    )
    parser.add_argument("--api-key")
    parser.add_argument("--api-key-file", default="/root/syn_sig_ra_api_key")
    parser.add_argument("--project-id", default=os.environ.get("SYN_SIG_RA_PROJECT_ID"))
    parser.add_argument("--pack-id", action="append", default=[])
    parser.add_argument("--poll-seconds", type=float, default=2.0)
    parser.add_argument("--job-timeout-seconds", type=float, default=900.0)
    parser.add_argument("--http-timeout-seconds", type=int, default=120)
    parser.add_argument("--download-packages", action="store_true")
    parser.add_argument("--keep-jobs", action="store_true")
    args = parser.parse_args()

    api_key = read_api_key(args.api_key, args.api_key_file)
    client = Client(args.base_url, api_key, args.http_timeout_seconds)
    project_id = choose_project(client, args.project_id)
    packs = list_packs(client, args.pack_id)
    if not packs:
        raise RuntimeError("No packs selected")

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="syn_sig_ra_pack_stress_") as tmp:
        tmp_path = pathlib.Path(tmp)
        print(f"base_url={args.base_url}", flush=True)
        print(f"project_id={project_id}", flush=True)
        print(f"packs={len(packs)}", flush=True)
        print(f"workdir={tmp}", flush=True)
        for index, pack in enumerate(packs, start=1):
            pack_id = pack.get("pack_id")
            if not isinstance(pack_id, str) or not pack_id:
                failures.append(f"pack #{index}: missing pack_id")
                continue
            print(f"[{index}/{len(packs)}] {pack_id}: queue", flush=True)
            job_id = ""
            try:
                queued = client.request_json(
                    "POST",
                    "/v1/jobs",
                    {"project_id": project_id, "pack_id": pack_id},
                )
                job_id = queued.get("job_id")
                if not isinstance(job_id, str) or not job_id:
                    raise RuntimeError("job create response did not contain job_id")
                job = wait_for_job(
                    client,
                    job_id,
                    args.poll_seconds,
                    args.job_timeout_seconds,
                )
                if job.get("status") != "succeeded":
                    raise RuntimeError(
                        "job ended as "
                        + str(job.get("status"))
                        + ": "
                        + json.dumps(job.get("error") or {}, sort_keys=True)
                    )
                validate_job(job, pack)
                package_id = job.get("package_id")
                if not isinstance(package_id, str) or not package_id:
                    raise RuntimeError("succeeded job has no package_id")

                pack_dir = tmp_path / pack_id
                pack_dir.mkdir()
                manifest_path = pack_dir / "manifest.json"
                manifest_bytes = client.download(
                    f"/v1/artifacts/{package_id}/manifest.json", manifest_path
                )
                validate_manifest(manifest_path)
                print(f"    manifest ok ({manifest_bytes} bytes)", flush=True)

                if args.download_packages:
                    archive_path = pack_dir / "package.zip"
                    archive_bytes = client.download(
                        f"/v1/artifacts/{package_id}/package.zip", archive_path
                    )
                    validate_zip(
                        archive_path,
                        [
                            "manifest.json",
                            "provenance.json",
                            "ENGINEERING_CLAIM_BOUNDARY.txt",
                        ],
                    )
                    print(f"    package.zip ok ({archive_bytes} bytes)", flush=True)

                kit_path = pack_dir / "verification-kit.zip"
                kit_bytes = client.download(
                    f"/v1/jobs/{job_id}/verification-kit.zip", kit_path
                )
                validate_kit(kit_path, job)
                print(f"    verification kit ok ({kit_bytes} bytes)", flush=True)
                print(f"[{index}/{len(packs)}] {pack_id}: ok", flush=True)
            except Exception as exc:  # noqa: BLE001 - stress script needs all failures.
                message = f"{pack_id}: {exc}"
                print(f"[{index}/{len(packs)}] {message}", file=sys.stderr, flush=True)
                failures.append(message)
            finally:
                if job_id and not args.keep_jobs:
                    delete_job(client, job_id)

    if failures:
        print("FAILURES:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("All selected packs passed.", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except KeyboardInterrupt:
        raise SystemExit(130)
