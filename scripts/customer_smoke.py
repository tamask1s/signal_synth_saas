#!/usr/bin/env python3
"""Dependency-free SynSigRa customer smoke client. Synthetic data only."""

import argparse
import json
import os
import pathlib
import time
import urllib.error
import urllib.request


def request(base, key, path, method="GET", body=None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    headers = {"Authorization": "Bearer " + key}
    if data is not None:
        headers["Content-Type"] = "application/json"
    call = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(call, timeout=30) as response:
            return response.read(), response.headers.get_content_type()
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", "replace")
        raise RuntimeError("HTTP {}: {}".format(error.code, detail)) from error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="https://www.timeonion.com/syn_sig_ra")
    parser.add_argument("--key", default=os.environ.get("SYN_SIG_RA_API_KEY"))
    parser.add_argument("--project")
    parser.add_argument("--pack", default="r_peak_stress_v1")
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("syn_sig_ra_output"))
    args = parser.parse_args()
    if not args.key:
        parser.error("--key or SYN_SIG_RA_API_KEY is required")

    projects = json.loads(request(args.base, args.key, "/v1/projects")[0])
    project_id = args.project or projects["projects"][0]["project_id"]
    created = json.loads(request(
        args.base, args.key, "/v1/jobs", "POST",
        {"project_id": project_id, "pack_id": args.pack},
    )[0])
    job_id = created["job_id"]
    for _ in range(120):
        job = json.loads(request(args.base, args.key, "/v1/jobs/" + job_id)[0])
        if job["status"] in ("succeeded", "failed", "cancelled"):
            break
        time.sleep(1)
    else:
        raise RuntimeError("job polling timed out")
    if job["status"] != "succeeded":
        raise RuntimeError("job ended with status " + job["status"])

    args.out.mkdir(parents=True, exist_ok=True)
    package_id = job["package_id"]
    for filename in ("manifest.json", "package.zip"):
        content, _ = request(
            args.base, args.key,
            "/v1/artifacts/{}/{}".format(package_id, filename),
        )
        (args.out / filename).write_bytes(content)
    print(json.dumps({"job_id": job_id, "package_id": package_id, "out": str(args.out)}))


if __name__ == "__main__":
    main()
