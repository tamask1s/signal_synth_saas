#!/usr/bin/env python3
"""Score user detector output against a downloaded SynSigRa package."""

import argparse
import json
import os
import pathlib

import synsigra as ss


def detection_path(root, case_id):
    for suffix in (".json", ".csv"):
        candidate = root / (case_id + suffix)
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("missing detection file for case {}".format(case_id))


def score_case(challenge, targets, case_id, detections, output):
    supported = [item for item in targets.get(case_id, []) if item in (
        "r_peak", "ppg_systolic_peak", "beat_classification"
    )]
    if not supported:
        raise ValueError("case {} has no supported detector target".format(case_id))
    target = supported[0]
    document = ss.load_detections(str(detections), target=target)
    case = challenge.case(case_id)
    case_output = output / case_id
    if target == "r_peak":
        report = ss.compare_rpeaks(case, document, out_dir=str(case_output))
    elif target == "ppg_systolic_peak":
        report = ss.compare_ppg_peaks(case, document, out_dir=str(case_output))
    else:
        report = ss.compare_beat_classes(case, document, out_dir=str(case_output))
    result = {
        "case_id": case_id,
        "target": target,
        "output": str(case_output),
        "summary": report.summary,
    }
    report.close()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("package", help="downloaded package.zip")
    parser.add_argument("detections", type=pathlib.Path,
                        help="directory containing <case-id>.csv or .json")
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--case", action="append", dest="cases",
                        help="score only this case ID; repeatable")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output directory already exists")

    with ss.load_challenge(args.package) as challenge:
        with open(challenge.resolve("pack.json"), "r") as handle:
            pack = json.load(handle)
        targets = dict(
            (item["id"], item.get("targets", pack.get("targets", [])))
            for item in pack.get("scenarios", [])
        )
        selected = args.cases or challenge.case_ids()
        args.output.mkdir(parents=True)
        results = []
        for case_id in selected:
            results.append(score_case(
                challenge,
                targets,
                case_id,
                detection_path(args.detections, case_id),
                args.output,
            ))
        with open(args.output / "verification_summary.json", "w") as handle:
            json.dump({"cases": results}, handle, indent=2, sort_keys=True)
    print("status=verified")
    print("case_count={}".format(len(results)))
    print("output={}".format(args.output))


if __name__ == "__main__":
    main()
