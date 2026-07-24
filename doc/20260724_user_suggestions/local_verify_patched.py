import datetime
import hashlib
import json
import math
import os
import shutil

from .challenge import ChallengePackage, load_challenge
from .detections import load_detections
from .delineation import delineation_scope_from_entry, delineation_truth_from_annotations, load_delineations, score_delineation_events
from .intervals import IntervalEvent, load_intervals, score_interval_events
from .measurements import MEASUREMENT_TARGETS, load_measurement_truth, load_measurements, score_measurements
from .profiles import load_threshold_profile
from .reporting import NOTICE_TEXT, annotate_policy, render_detail, render_index
from .submission import SubmissionError, load_submission


SCORING_VERSION = "synsigra-python-local-v9"
LIMITATION_TEXT = NOTICE_TEXT
BEAT_CLASSES = ["normal", "supraventricular_ectopic", "ventricular_ectopic", "paced", "escape", "fusion", "unscored"]
SCORED_BEAT_CLASSES = set(["normal", "supraventricular_ectopic", "ventricular_ectopic", "paced", "escape", "fusion"])
PPG_EVENT_TARGETS = frozenset(["ppg_systolic_peak", "ppg_pulse_onset"])
EVENT_TARGETS = frozenset(["r_peak", "ecg_beat_classification"]) | PPG_EVENT_TARGETS
INTERVAL_TARGETS = frozenset(["rhythm_episode", "signal_quality"])
SUPPORTED_TARGETS = EVENT_TARGETS | INTERVAL_TARGETS | MEASUREMENT_TARGETS | frozenset(["ecg_delineation"])


class VerificationError(ValueError):
    pass


class VerificationReport(object):
    def __init__(self, output_dir, evidence, evidence_path, entry_point):
        self.output_dir = output_dir
        self.evidence = evidence
        self.evidence_path = evidence_path
        self.entry_point = entry_point


class _TruthEvent(object):
    def __init__(self, index, time_seconds, label="", in_artifact_interval=False, in_motion_artifact_interval=False, in_dropout_artifact_interval=False, low_perfusion=False, weak_pulse=False, missing_pulse_window=False, scoreable=True, exclusion_reason=""):
        self.index = int(index)
        self.time_seconds = float(time_seconds)
        self.label = label
        self.in_artifact_interval = bool(in_artifact_interval)
        self.in_motion_artifact_interval = bool(in_motion_artifact_interval)
        self.in_dropout_artifact_interval = bool(in_dropout_artifact_interval)
        self.low_perfusion = bool(low_perfusion)
        self.weak_pulse = bool(weak_pulse)
        self.missing_pulse_window = bool(missing_pulse_window)
        self.scoreable = bool(scoreable)
        self.exclusion_reason = str(exclusion_reason or "")


def verify_package(challenge, submission_dir, out_dir, cases=None, targets=None, mode="evidence", profile=None, force=False):
    """Verify a declared user submission against a Synsigra challenge locally.

    This verifier uses only challenge package contents: manifest metadata,
    annotations, case summaries, and declared user output files. It does not
    invoke the signal generator or require generator source code.
    """
    package, owned = _as_package(challenge)
    try:
        integrity = package.verify_integrity()
        scoring_manifest = package.scoring_manifest()
        verification = _verification_configuration(package, scoring_manifest, mode, cases, targets, profile)
        try:
            submission = load_submission(submission_dir, package, scoring_manifest)
        except SubmissionError as error:
            raise VerificationError(str(error))
        threshold_profile = verification.pop("threshold_profile")
        _prepare_output_dir(out_dir, force)
        selected_cases = verification.pop("selected_cases")
        selected_targets = verification.pop("selected_targets")
        acceptance_strata = verification.pop("acceptance_strata")
        required_matrix = verification["required_matrix"]
        results = []
        messages = []

        for case in package.cases:
            if selected_cases is not None and case.id not in selected_cases:
                continue
            case_summary = case.case_summary()
            annotations = case.annotations()
            entries = _case_scoring_entries(scoring_manifest, case.id, case_summary)
            for entry in entries:
                target = entry.get("target", "")
                if mode == "evidence" and (case.id, target) not in required_matrix:
                    continue
                if selected_targets is not None and target not in selected_targets:
                    continue
                if not entry.get("supported", False):
                    results.append(_unsupported_result(case, case_summary, entry))
                    continue
                if target not in SUPPORTED_TARGETS:
                    results.append(_unsupported_result(case, case_summary, entry))
                    continue
                result = _verify_case_target(package, case, case_summary, annotations, entry, submission)
                results.append(result)

        if not results:
            messages.append("no scoreable case-target pairs matched the selected filters")
        if mode == "diagnostic":
            messages.append("diagnostic mode is exploratory and is not eligible for package-protocol evidence")

        evidence = _build_evidence_record(package, scoring_manifest, submission, integrity, results, messages, threshold_profile, acceptance_strata, verification)
        json_path, html_path = _write_verification_bundle(out_dir, evidence)
        return VerificationReport(out_dir, evidence, json_path, html_path)
    finally:
        if owned:
            package.close()


def _verification_configuration(package, scoring_manifest, mode, cases, targets, profile):
    if mode not in ("evidence", "diagnostic"):
        raise VerificationError("verification mode must be evidence or diagnostic")
    _validate_scoring_manifest_contract(package, scoring_manifest)
    selected_cases = _normalize_filter(cases)
    selected_targets = _normalize_filter(targets)
    protocol = None
    protocol_identity = None
    try:
        protocol = package.verification_protocol()
        protocol_identity = package.verification_protocol_identity()
    except KeyError:
        pass
    required_matrix = _scoring_matrix(scoring_manifest)
    if protocol is not None:
        verdict_scope = protocol.get("verdict_scope", "aggregate")
        protocol_matrix = _protocol_matrix(protocol)
        if protocol_matrix != required_matrix:
            missing = sorted(required_matrix - protocol_matrix)
            extra = sorted(protocol_matrix - required_matrix)
            raise VerificationError("verification protocol case-target matrix does not equal the scoring manifest (missing=%s, extra=%s)" % (missing, extra))
        unsupported = sorted(set(target for _case_id, target in protocol_matrix if target not in SUPPORTED_TARGETS))
        if unsupported:
            raise VerificationError("verification protocol requires unsupported targets: %s" % ", ".join(unsupported))
        unknown_cases = sorted(set(case_id for case_id, _target in protocol_matrix) - set(package.case_ids()))
        if unknown_cases:
            raise VerificationError("verification protocol references cases absent from the challenge package: %s" % ", ".join(unknown_cases))
        protocol_identity.update({
            "scoring_contract": protocol["scoring_contract"],
            "context_of_use": protocol["context_of_use"],
            "acceptance_profile_id": (
                protocol["acceptance_profile"]["profile_id"]
                if verdict_scope == "aggregate"
                else "per_case_profiles"
            ),
            "verdict_scope": verdict_scope,
            "evidence_boundary": protocol["evidence_boundary"],
            "truth_policy": dict(protocol["truth_policy"]),
        })
    if mode == "evidence":
        if protocol is None:
            raise VerificationError("evidence mode requires a packaged synsigra_verification_protocol_v2 artifact")
        if selected_cases is not None or selected_targets is not None or profile is not None:
            raise VerificationError("evidence mode forbids case filters, target filters, and caller-selected threshold profiles")
        if protocol.get("verdict_scope", "aggregate") == "per_case":
            threshold_profile = {
                "schema_version": 1,
                "profile_id": "per_case_profiles",
                "description": "Each case has its own official acceptance profile; no cases are pooled for a verdict.",
                "targets": {},
            }
        else:
            threshold_profile = load_threshold_profile(protocol["acceptance_profile"])
        acceptance_strata = [
            {
                "id": item["id"],
                "case_ids": list(item["case_ids"]),
                "profile": load_threshold_profile(item["acceptance_profile"]),
            }
            for item in protocol.get("acceptance_strata", [])
        ]
    else:
        default_profile = (
            protocol["acceptance_profile"]
            if protocol is not None and protocol.get("verdict_scope", "aggregate") == "aggregate"
            else "regression"
        )
        threshold_profile = load_threshold_profile(profile if profile is not None else default_profile)
        acceptance_strata = []
    return {
        "mode": mode,
        "protocol": protocol_identity,
        "required_matrix": required_matrix if protocol is not None else set(),
        "selected_cases": selected_cases,
        "selected_targets": selected_targets,
        "threshold_profile": threshold_profile,
        "acceptance_strata": acceptance_strata,
    }


def _validate_scoring_manifest_contract(package, scoring_manifest):
    if not isinstance(scoring_manifest, dict):
        raise VerificationError("scoring manifest must be a JSON object")
    if isinstance(scoring_manifest.get("schema_version"), bool) or scoring_manifest.get("schema_version") != 3:
        raise VerificationError("scoring manifest schema_version must be 3")
    if scoring_manifest.get("contract") != "synsigra_scoring_manifest_v3" or scoring_manifest.get("scoring_manifest_contract_version") != "synsigra_scoring_manifest_v3":
        raise VerificationError("scoring manifest contract is incompatible with this verifier")
    if scoring_manifest.get("package_id") != package.package_id:
        raise VerificationError("scoring manifest package_id does not match the challenge package")
    cases = scoring_manifest.get("cases")
    if not isinstance(cases, list) or not cases:
        raise VerificationError("scoring manifest cases must be a non-empty array")
    known_cases = set(package.case_ids())
    seen_cases = set()
    matrix_targets = set()
    for case_index, case in enumerate(cases):
        if not isinstance(case, dict) or not isinstance(case.get("case_id"), str) or not case["case_id"]:
            raise VerificationError("scoring manifest case %d has an invalid case_id" % case_index)
        case_id = case["case_id"]
        if case_id in seen_cases:
            raise VerificationError("scoring manifest contains duplicate case_id: %s" % case_id)
        if case_id not in known_cases:
            raise VerificationError("scoring manifest references a case absent from the challenge package: %s" % case_id)
        seen_cases.add(case_id)
        entries = case.get("scoring")
        if not isinstance(entries, list):
            raise VerificationError("scoring manifest case %s scoring must be an array" % case_id)
        case_targets = set()
        for entry_index, entry in enumerate(entries):
            if not isinstance(entry, dict) or not isinstance(entry.get("target"), str) or not entry["target"]:
                raise VerificationError("scoring manifest case %s entry %d has an invalid target" % (case_id, entry_index))
            if type(entry.get("supported")) is not bool:
                raise VerificationError("scoring manifest case %s target %s has a non-boolean supported flag" % (case_id, entry["target"]))
            if entry["target"] in case_targets:
                raise VerificationError("scoring manifest contains duplicate case-target: %s/%s" % (case_id, entry["target"]))
            case_targets.add(entry["target"])
            matrix_targets.add(entry["target"])
    if seen_cases != known_cases:
        raise VerificationError("scoring manifest does not enumerate every challenge case")
    targets = scoring_manifest.get("targets")
    if not isinstance(targets, list):
        raise VerificationError("scoring manifest targets do not equal its case target union")
    target_names = []
    for target_index, target in enumerate(targets):
        if not isinstance(target, dict) or not isinstance(target.get("target"), str) or not target["target"]:
            raise VerificationError("scoring manifest target %d is invalid" % target_index)
        target_names.append(target["target"])
    if len(set(target_names)) != len(target_names) or set(target_names) != matrix_targets:
        raise VerificationError("scoring manifest targets do not equal its case target union")


def _scoring_matrix(scoring_manifest):
    matrix = set()
    for case in scoring_manifest.get("cases", []):
        case_id = case.get("case_id", "")
        for entry in case.get("scoring", []):
            if entry.get("supported", False):
                key = (case_id, entry.get("target", ""))
                if key in matrix:
                    raise VerificationError("scoring manifest contains duplicate case-target: %s/%s" % key)
                matrix.add(key)
    if not matrix:
        raise VerificationError("scoring manifest has no supported case-target matrix")
    return matrix


def _protocol_matrix(protocol):
    return set((item["case_id"], target) for item in protocol["required_case_targets"] for target in item["targets"])


def _verify_case_target(package, case, case_summary, annotations, entry, submission):
    target = entry.get("target", "")
    relative_out = _target_output_relative_path(case.id, case_summary.get("targets", []), target)
    submitted = submission.output(case.id, target)
    if submitted is None:
        return _error_result(package, case, case_summary, entry, relative_out, "missing_submission_entry", "submission has no output entry for %s/%s" % (case.id, target))
    detection_path = submission.path(submitted)
    if not os.path.isfile(detection_path):
        return _error_result(package, case, case_summary, entry, relative_out, "missing_output", "submission output file is missing for %s/%s: %s" % (case.id, target, submitted.relative_path))
    algorithm = dict(submission.algorithm)
    try:
        if target in MEASUREMENT_TARGETS:
            predictions = load_measurements(detection_path, format_name=submitted.format)
            truth_path = entry.get("ground_truth_path", "")
            if not truth_path:
                raise VerificationError("measurement scoring entry has no ground_truth_path")
            truth = load_measurement_truth(package.resolve(truth_path), target)
            report_json = score_measurements(truth, predictions, target, float(entry.get("default_pairing_window_seconds", 0.2)))
            report_json["verifier_version"] = SCORING_VERSION
            report_json["package"] = _package_identity(package)
            report_json["scenario"] = _scenario_identity(case, case_summary, annotations)
            report_json["algorithm"] = algorithm
            report_json["notes"] = ["Undefined, absent, and not-evaluable states are explicit and are not coerced to numeric zero.", LIMITATION_TEXT]
            identity = _submission_output_identity(detection_path, submitted.relative_path, submitted.format, target, algorithm, "measurement_count", len(predictions))
            report_json["submission_output"] = identity
            return _case_result_from_report(case, case_summary, target, relative_out, detection_path, identity, report_json)
        if target in INTERVAL_TARGETS:
            intervals = load_intervals(detection_path, target=target, format_name=submitted.format)
            report_json = _score_interval_detection(package, case, case_summary, annotations, intervals, entry)
            report_json["algorithm"] = algorithm
            identity = _submission_output_identity(detection_path, submitted.relative_path, submitted.format, target, algorithm, "interval_count", len(intervals))
            report_json["submission_output"] = identity
            return _case_result_from_report(case, case_summary, target, relative_out, detection_path, identity, report_json)
        if target == "ecg_delineation":
            delineations = load_delineations(detection_path, format_name=submitted.format)
            report_json = _score_delineation(package, case, case_summary, annotations, delineations, entry)
            report_json["algorithm"] = algorithm
            identity = _submission_output_identity(detection_path, submitted.relative_path, submitted.format, target, algorithm, "event_count", len(delineations))
            report_json["submission_output"] = identity
            return _case_result_from_report(case, case_summary, target, relative_out, detection_path, identity, report_json)
        detections = load_detections(detection_path, target=target, format_name=submitted.format)
        if target == "ecg_beat_classification":
            report_json = _score_beat_classification(package, case, case_summary, annotations, detections, entry)
        else:
            report_json = _score_event_detection(package, case, case_summary, annotations, detections, entry)
        identity = _submission_output_identity(detection_path, submitted.relative_path, submitted.format, target, algorithm, "event_count", len(detections))
        report_json["algorithm"] = algorithm
        report_json["submission_output"] = identity
        return _case_result_from_report(case, case_summary, target, relative_out, detection_path, identity, report_json)
    except Exception as error:
        message = "%s/%s: %s" % (case.id, target, str(error))
        return _error_result(package, case, case_summary, entry, relative_out, "scoring_error", message, detection_path)


def _score_interval_detection(package, case, case_summary, annotations, intervals, entry):
    target = intervals.target
    minimum_iou = float(entry.get("default_minimum_iou", 0.1))
    predictions = list(intervals.intervals)
    channel_mode = "global"
    if target == "signal_quality":
        has_global = any(item.channel == "global" for item in predictions)
        has_physical = any(item.channel != "global" for item in predictions)
        if has_global and has_physical:
            raise VerificationError("signal_quality predictions cannot mix global and physical channels")
        channel_mode = "per_channel" if has_physical else "global"
    truth = _truth_intervals_for_target(target, annotations, case_summary, channel_mode)
    duration_seconds = float(case_summary.get("render", {}).get("duration_seconds", annotations.get("duration_seconds", 0.0)))
    comparison = score_interval_events(truth, predictions, duration_seconds, minimum_iou)
    scenario_identity = _scenario_identity(case, case_summary, annotations)
    scenario_identity["duration_seconds"] = duration_seconds
    return {
        "schema_version": 1,
        "score_type": "interval_detection_qa",
        "scoring_version": SCORING_VERSION,
        "package": _package_identity(package),
        "scenario": scenario_identity,
        "target": target,
        "algorithm": {"name": intervals.algorithm_name, "version": intervals.algorithm_version},
        "options": {"minimum_iou": minimum_iou, "channel_mode": channel_mode},
        "overall": comparison["overall"],
        "classes": comparison["classes"],
        "confusion_matrix": comparison["confusion_matrix"],
        "matches": comparison["matches"],
        "false_positive_indices": comparison["false_positive_indices"],
        "false_negative_indices": comparison["false_negative_indices"],
        "notes": ["Intervals are half-open [start,end); undefined zero-denominator metrics are null.", LIMITATION_TEXT],
    }


def _score_delineation(package, case, case_summary, annotations, delineations, entry):
    tolerance_seconds = float(entry.get("default_tolerance_seconds", 0.040))
    pairing_window_seconds = float(entry.get("default_pairing_window_seconds", 0.200))
    duration_seconds = float(case_summary.get("render", {}).get("duration_seconds", annotations.get("duration_seconds", 0.0)))
    scope = delineation_scope_from_entry(entry)
    truth = delineation_truth_from_annotations(annotations, scope, duration_seconds)
    comparison = score_delineation_events(truth, delineations.events, duration_seconds, scope, tolerance_seconds, pairing_window_seconds)
    scenario_identity = _scenario_identity(case, case_summary, annotations)
    scenario_identity["duration_seconds"] = duration_seconds
    return {
        "schema_version": 2,
        "score_type": "ecg_delineation_qa",
        "scoring_version": SCORING_VERSION,
        "package": _package_identity(package),
        "scenario": scenario_identity,
        "target": "ecg_delineation",
        "scope": {"mode": "selected_windows" if scope.windows else "all_record", "leads": list(scope.leads), "windows": [{"start_seconds": item[0], "end_seconds": item[1]} for item in scope.windows]},
        "options": {"tolerance_seconds": tolerance_seconds, "pairing_window_seconds": pairing_window_seconds},
        "overall": comparison["overall"],
        "by_kind": comparison["by_kind"],
        "by_lead": comparison["by_lead"],
        "by_kind_lead": comparison["by_kind_lead"],
        "truth": comparison["truth"],
        "matches": comparison["matches"],
        "missing_events": comparison["missing_events"],
        "unexpected_events": comparison["unexpected_events"],
        "excluded_predictions": comparison["excluded_predictions"],
        "notes": ["Predictions are paired by lead, kind, and time; generator anchor identities remain truth/report metadata.", LIMITATION_TEXT],
    }


def _truth_intervals_for_target(target, annotations, case_summary, channel_mode):
    truth = []
    if target == "rhythm_episode":
        for item in annotations.get("episodes", []):
            if item.get("present", True) and item.get("kind") in ("psvt", "svarr"):
                truth.append(IntervalEvent(item["start_seconds"], item["end_seconds"], item["kind"], "global", original_index=len(truth)))
        return truth
    artifact_intervals = annotations.get("artifact_intervals")
    if artifact_intervals is None:
        artifact_intervals = case_summary.get("artifact_intervals", [])
    for item in artifact_intervals:
        channels = ["global"] if channel_mode == "global" else list(item.get("channels", []))
        for channel in channels:
            truth.append(IntervalEvent(item["start_seconds"], item["end_seconds"], item["type"], channel, original_index=len(truth)))
    return truth


def _score_event_detection(package, case, case_summary, annotations, detections, entry):
    target = entry.get("target", "")
    tolerance_seconds = float(entry.get("default_tolerance_seconds", _default_tolerance_seconds(target)))
    truth = _truth_events_for_target(target, annotations, case_summary)
    detection_events = []
    ppg_target = target in PPG_EVENT_TARGETS
    for item in detections.events:
        if not _finite_non_negative(item.time_seconds):
            raise VerificationError("detection time must be finite and non-negative")
        pulse = _ppg_pulse_at_time(annotations, item.time_seconds) if ppg_target else None
        detection_events.append(_TruthEvent(
            item.original_index, item.time_seconds, "", _in_artifact_interval(target, item.time_seconds, annotations, case_summary),
            _in_motion_artifact_interval(target, item.time_seconds, annotations, case_summary),
            _in_dropout_artifact_interval(target, item.time_seconds, annotations, case_summary),
            bool(pulse and pulse.get("low_perfusion", False)),
            bool(pulse and _ppg_pulse_state(pulse) == "weak"),
            bool(pulse and _ppg_pulse_state(pulse) == "missing")))
    missing_pulse_opportunity_count = sum(1 for item in annotations.get("ppg_pulses", []) if _ppg_pulse_state(item) == "missing") if ppg_target else 0
    result = _compare_events(target, truth, detection_events, tolerance_seconds, missing_pulse_opportunity_count)
    return {
        "schema_version": 1,
        "score_type": "event_detection_qa",
        "scoring_version": SCORING_VERSION,
        "package": _package_identity(package),
        "scenario": _scenario_identity(case, case_summary, annotations),
        "comparison": result,
        "truth_policy": dict(annotations.get("truth_policy", {})),
        "notes": [LIMITATION_TEXT],
    }


def _score_beat_classification(package, case, case_summary, annotations, detections, entry):
    tolerance_seconds = float(entry.get("default_tolerance_seconds", _default_tolerance_seconds("ecg_beat_classification")))
    truth = _truth_events_for_target("ecg_beat_classification", annotations, case_summary)
    predictions = []
    for item in detections.events:
        if not _finite_non_negative(item.time_seconds):
            raise VerificationError("classification event time must be finite and non-negative")
        if item.label not in BEAT_CLASSES:
            raise VerificationError("classification event label is not a canonical ECG beat class: %s" % item.label)
        predictions.append(_TruthEvent(item.original_index, item.time_seconds, item.label, False))
    result = _score_classification_events(truth, predictions, tolerance_seconds)
    result.update({
        "schema_version": 1,
        "score_type": "ecg_beat_classification_qa",
        "scoring_version": SCORING_VERSION,
        "scenario_id": case_summary.get("scenario_id", case.scenario_id),
        "document_fingerprint": case_summary.get("document_fingerprint", case.document_fingerprint),
        "render_identity": case_summary.get("render_identity", case.render_identity),
        "package": _package_identity(package),
        "scenario": _scenario_identity(case, case_summary, annotations),
        "algorithm": {"name": detections.algorithm_name, "version": detections.algorithm_version},
        "intended_use": "synthetic engineering algorithm testing and QA",
        "not_for": "diagnosis, patient monitoring, clinical validation certification, or standalone conformity assessment",
    })
    return result


def _compare_events(target, truth, detections, tolerance_seconds, missing_pulse_opportunity_count=0):
    if not truth and target in PPG_EVENT_TARGETS:
        raise VerificationError("scenario has no requested PPG event ground truth")
    excluded_truth = [item for item in truth if not item.scoreable]
    truth = [item for item in truth if item.scoreable]
    sorted_detections = sorted(detections, key=lambda item: (item.time_seconds, item.index))
    candidates = []
    for truth_index, truth_event in enumerate(truth):
        for detection_sorted_index, detection_event in enumerate(sorted_detections):
            absolute_error = abs(detection_event.time_seconds - truth_event.time_seconds)
            if absolute_error <= tolerance_seconds:
                candidates.append((absolute_error, truth_index, detection_sorted_index))
    candidates.sort()
    truth_matched = [False] * len(truth)
    detection_matched = [False] * len(sorted_detections)
    detection_excluded = [False] * len(sorted_detections)
    matched_detection_times = [0.0] * len(truth)
    total = _empty_event_metrics()
    clean = _empty_event_metrics()
    artifact = _empty_event_metrics()
    motion = _empty_event_metrics()
    dropout = _empty_event_metrics()
    low_perfusion = _empty_event_metrics()
    weak = _empty_event_metrics()
    total_errors = []
    clean_errors = []
    artifact_errors = []
    motion_errors = []
    dropout_errors = []
    low_perfusion_errors = []
    weak_errors = []
    matches = []
    false_positives = []
    false_negatives = []
    for absolute_error, truth_index, detection_sorted_index in candidates:
        if truth_matched[truth_index] or detection_matched[detection_sorted_index]:
            continue
        truth_matched[truth_index] = True
        detection_matched[detection_sorted_index] = True
        matched_detection_times[truth_index] = sorted_detections[detection_sorted_index].time_seconds
        truth_event = truth[truth_index]
        detection_event = sorted_detections[detection_sorted_index]
        error_seconds = detection_event.time_seconds - truth_event.time_seconds
        match = {
            "ground_truth_index": truth_event.index,
            "detection_index": detection_event.index,
            "ground_truth_time_seconds": truth_event.time_seconds,
            "detection_time_seconds": detection_event.time_seconds,
            "error_seconds": error_seconds,
            "in_artifact_interval": truth_event.in_artifact_interval,
            "in_motion_artifact_interval": truth_event.in_motion_artifact_interval,
            "in_dropout_artifact_interval": truth_event.in_dropout_artifact_interval,
            "low_perfusion": truth_event.low_perfusion,
            "weak_pulse": truth_event.weak_pulse,
        }
        matches.append(match)
        total["true_positive_count"] += 1
        total_errors.append(abs(error_seconds))
        if truth_event.in_artifact_interval:
            artifact["true_positive_count"] += 1
            artifact_errors.append(abs(error_seconds))
        else:
            clean["true_positive_count"] += 1
            clean_errors.append(abs(error_seconds))
        if truth_event.low_perfusion:
            low_perfusion["true_positive_count"] += 1
            low_perfusion_errors.append(abs(error_seconds))
        if truth_event.in_motion_artifact_interval:
            motion["true_positive_count"] += 1
            motion_errors.append(abs(error_seconds))
        if truth_event.in_dropout_artifact_interval:
            dropout["true_positive_count"] += 1
            dropout_errors.append(abs(error_seconds))
        if truth_event.weak_pulse:
            weak["true_positive_count"] += 1
            weak_errors.append(abs(error_seconds))

    excluded_detections = []
    for detection_index, detection_event in enumerate(sorted_detections):
        if detection_matched[detection_index]:
            continue
        candidates = [
            item for item in excluded_truth
            if abs(detection_event.time_seconds - item.time_seconds) <= tolerance_seconds
        ]
        if not candidates:
            continue
        nearest = min(candidates, key=lambda item: (abs(detection_event.time_seconds - item.time_seconds), item.index))
        detection_excluded[detection_index] = True
        excluded_detections.append({
            "detection_index": detection_event.index,
            "time_seconds": detection_event.time_seconds,
            "excluded_ground_truth_index": nearest.index,
            "excluded_ground_truth_time_seconds": nearest.time_seconds,
            "reason": nearest.exclusion_reason,
        })

    for index, truth_event in enumerate(truth):
        _bin_metrics(truth_event.in_artifact_interval, clean, artifact)["ground_truth_count"] += 1
        if truth_event.in_motion_artifact_interval:
            motion["ground_truth_count"] += 1
        if truth_event.in_dropout_artifact_interval:
            dropout["ground_truth_count"] += 1
        if truth_event.low_perfusion:
            low_perfusion["ground_truth_count"] += 1
        if truth_event.weak_pulse:
            weak["ground_truth_count"] += 1
        if not truth_matched[index]:
            event = {
                "ground_truth_index": truth_event.index, "time_seconds": truth_event.time_seconds,
                "in_artifact_interval": truth_event.in_artifact_interval,
                "in_motion_artifact_interval": truth_event.in_motion_artifact_interval,
                "in_dropout_artifact_interval": truth_event.in_dropout_artifact_interval,
                "low_perfusion": truth_event.low_perfusion, "weak_pulse": truth_event.weak_pulse,
            }
            false_negatives.append(event)
            total["false_negative_count"] += 1
            _bin_metrics(truth_event.in_artifact_interval, clean, artifact)["false_negative_count"] += 1
            if truth_event.low_perfusion:
                low_perfusion["false_negative_count"] += 1
            if truth_event.weak_pulse:
                weak["false_negative_count"] += 1
            if truth_event.in_motion_artifact_interval:
                motion["false_negative_count"] += 1
            if truth_event.in_dropout_artifact_interval:
                dropout["false_negative_count"] += 1
    for index, detection_event in enumerate(sorted_detections):
        if detection_excluded[index]:
            continue
        _bin_metrics(detection_event.in_artifact_interval, clean, artifact)["detection_count"] += 1
        if detection_event.in_motion_artifact_interval:
            motion["detection_count"] += 1
        if detection_event.in_dropout_artifact_interval:
            dropout["detection_count"] += 1
        if detection_event.low_perfusion:
            low_perfusion["detection_count"] += 1
        if detection_event.weak_pulse:
            weak["detection_count"] += 1
        if not detection_matched[index]:
            event = {
                "detection_index": detection_event.index, "time_seconds": detection_event.time_seconds,
                "in_artifact_interval": detection_event.in_artifact_interval,
                "in_motion_artifact_interval": detection_event.in_motion_artifact_interval,
                "in_dropout_artifact_interval": detection_event.in_dropout_artifact_interval,
                "low_perfusion": detection_event.low_perfusion, "weak_pulse": detection_event.weak_pulse,
                "missing_pulse_window": detection_event.missing_pulse_window,
            }
            false_positives.append(event)
            total["false_positive_count"] += 1
            _bin_metrics(detection_event.in_artifact_interval, clean, artifact)["false_positive_count"] += 1
            if detection_event.low_perfusion:
                low_perfusion["false_positive_count"] += 1
            if detection_event.weak_pulse:
                weak["false_positive_count"] += 1
            if detection_event.in_motion_artifact_interval:
                motion["false_positive_count"] += 1
            if detection_event.in_dropout_artifact_interval:
                dropout["false_positive_count"] += 1
    total["ground_truth_count"] = len(truth)
    total["detection_count"] = len(detections) - len(excluded_detections)
    total["excluded_ground_truth_count"] = len(excluded_truth)
    total["excluded_detection_count"] = len(excluded_detections)
    _finalize_event_metrics(total, total_errors)
    _finalize_event_metrics(clean, clean_errors)
    _finalize_event_metrics(artifact, artifact_errors)
    _finalize_event_metrics(motion, motion_errors)
    _finalize_event_metrics(dropout, dropout_errors)
    _finalize_event_metrics(low_perfusion, low_perfusion_errors)
    _finalize_event_metrics(weak, weak_errors)
    scored_detections = [item for index, item in enumerate(sorted_detections) if not detection_excluded[index]]
    pulse_timing = _ppg_pulse_timing_metrics(target, truth, scored_detections, truth_matched, matched_detection_times)
    return {
        "target": target,
        "tolerance_seconds": tolerance_seconds,
        "success": True,
        "metrics": {
            "total": total, "clean": clean, "artifact": artifact, "motion": motion, "dropout": dropout,
            "low_perfusion": low_perfusion, "weak": weak,
            "missing_pulse": {
                "opportunity_count": missing_pulse_opportunity_count,
                "detection_count": sum(1 for item in detections if item.missing_pulse_window),
            },
            "pulse_timing": pulse_timing,
        },
        "matches": matches,
        "false_positives": false_positives,
        "false_negatives": false_negatives,
        "excluded_ground_truth": [
            {"ground_truth_index": item.index, "time_seconds": item.time_seconds, "reason": item.exclusion_reason}
            for item in excluded_truth
        ],
        "excluded_detections": excluded_detections,
    }


def _ppg_pulse_timing_metrics(target, truth, detections, truth_matched, matched_detection_times):
    result = {
        "ground_truth_interval_count": 0, "detection_interval_count": 0, "matched_interval_count": 0,
        "mean_absolute_interval_error_seconds": 0.0, "rms_interval_error_seconds": 0.0,
        "max_absolute_interval_error_seconds": 0.0, "ground_truth_mean_pulse_rate_bpm": 0.0,
        "detection_mean_pulse_rate_bpm": 0.0, "absolute_pulse_rate_error_bpm": 0.0,
    }
    if target not in PPG_EVENT_TARGETS:
        return result
    result["ground_truth_interval_count"] = max(0, len(truth) - 1)
    result["detection_interval_count"] = max(0, len(detections) - 1)
    if len(truth) > 1 and truth[-1].time_seconds > truth[0].time_seconds:
        result["ground_truth_mean_pulse_rate_bpm"] = 60.0 * (len(truth) - 1) / (truth[-1].time_seconds - truth[0].time_seconds)
    if len(detections) > 1 and detections[-1].time_seconds > detections[0].time_seconds:
        result["detection_mean_pulse_rate_bpm"] = 60.0 * (len(detections) - 1) / (detections[-1].time_seconds - detections[0].time_seconds)
    result["absolute_pulse_rate_error_bpm"] = abs(result["detection_mean_pulse_rate_bpm"] - result["ground_truth_mean_pulse_rate_bpm"])
    errors = []
    for index in range(1, len(truth)):
        if truth_matched[index - 1] and truth_matched[index]:
            truth_interval = truth[index].time_seconds - truth[index - 1].time_seconds
            detection_interval = matched_detection_times[index] - matched_detection_times[index - 1]
            errors.append(abs(detection_interval - truth_interval))
    result["matched_interval_count"] = len(errors)
    if errors:
        result["mean_absolute_interval_error_seconds"] = sum(errors) / len(errors)
        result["rms_interval_error_seconds"] = math.sqrt(sum(item * item for item in errors) / len(errors))
        result["max_absolute_interval_error_seconds"] = max(errors)
    return result


def _score_classification_events(truth, predictions, tolerance_seconds):
    candidates = []
    for truth_index, truth_event in enumerate(truth):
        for prediction_index, prediction in enumerate(predictions):
            error = abs(prediction.time_seconds - truth_event.time_seconds)
            if error <= tolerance_seconds:
                candidates.append((error, truth_index, prediction_index))
    candidates.sort()
    truth_used = [False] * len(truth)
    prediction_used = [False] * len(predictions)
    classes = dict((name, _empty_class_metrics(name != "unscored")) for name in BEAT_CLASSES)
    confusion = dict((actual, dict((predicted, 0) for predicted in BEAT_CLASSES)) for actual in BEAT_CLASSES)
    scored_ground_truth_count = 0
    scored_prediction_count = 0
    matched_count = 0
    correct_count = 0
    unscored_match_count = 0
    timing_error_sum = 0.0
    max_absolute_error_seconds = 0.0
    matches = []
    unmatched_ground_truth = []
    unmatched_predictions = []

    for event in truth:
        classes[event.label]["ground_truth_count"] += 1
        if event.label in SCORED_BEAT_CLASSES:
            scored_ground_truth_count += 1

    for absolute_error, truth_index, prediction_index in candidates:
        if truth_used[truth_index] or prediction_used[prediction_index]:
            continue
        truth_used[truth_index] = True
        prediction_used[prediction_index] = True
        truth_event = truth[truth_index]
        prediction = predictions[prediction_index]
        actual = truth_event.label
        predicted = prediction.label
        scored = actual in SCORED_BEAT_CLASSES
        correct = scored and actual == predicted
        confusion[actual][predicted] += 1
        classes[predicted]["prediction_count"] += 1
        matched_count += 1
        if not scored:
            unscored_match_count += 1
        else:
            timing_error_sum += absolute_error
            max_absolute_error_seconds = max(max_absolute_error_seconds, absolute_error)
            if predicted in SCORED_BEAT_CLASSES:
                scored_prediction_count += 1
            if correct:
                correct_count += 1
                classes[actual]["true_positive_count"] += 1
            else:
                classes[actual]["false_negative_count"] += 1
                if predicted in SCORED_BEAT_CLASSES:
                    classes[predicted]["false_positive_count"] += 1
        matches.append({
            "ground_truth_index": truth_event.index,
            "prediction_index": prediction.index,
            "ground_truth_time_seconds": truth_event.time_seconds,
            "prediction_time_seconds": prediction.time_seconds,
            "error_seconds": prediction.time_seconds - truth_event.time_seconds,
            "actual_class": actual,
            "predicted_class": predicted,
            "scored": scored,
            "correct": correct,
            "exclusion_reason": truth_event.exclusion_reason,
        })

    for index, truth_event in enumerate(truth):
        if truth_used[index]:
            continue
        unmatched_ground_truth.append({"ground_truth_index": truth_event.index, "time_seconds": truth_event.time_seconds, "actual_class": truth_event.label, "exclusion_reason": truth_event.exclusion_reason})
        if truth_event.label in SCORED_BEAT_CLASSES:
            classes[truth_event.label]["false_negative_count"] += 1
    for index, prediction in enumerate(predictions):
        if prediction_used[index]:
            continue
        unmatched_predictions.append({"prediction_index": prediction.index, "time_seconds": prediction.time_seconds, "predicted_class": prediction.label})
        classes[prediction.label]["prediction_count"] += 1
        if prediction.label in SCORED_BEAT_CLASSES:
            scored_prediction_count += 1
            classes[prediction.label]["false_positive_count"] += 1

    for metrics in classes.values():
        if not metrics["scored"]:
            continue
        metrics["precision"] = _ratio(metrics["true_positive_count"], metrics["true_positive_count"] + metrics["false_positive_count"])
        metrics["recall"] = _ratio(metrics["true_positive_count"], metrics["true_positive_count"] + metrics["false_negative_count"])
        metrics["f1_score"] = _f1(metrics["precision"], metrics["recall"])
    scored_matches = matched_count - unscored_match_count
    micro_precision = _ratio(correct_count, scored_prediction_count)
    micro_recall = _ratio(correct_count, scored_ground_truth_count)
    summary = {
        "scored_ground_truth_count": scored_ground_truth_count,
        "scored_prediction_count": scored_prediction_count,
        "matched_count": matched_count,
        "correct_count": correct_count,
        "unscored_match_count": unscored_match_count,
        "excluded_ground_truth_count": sum(1 for item in truth if not item.scoreable),
        "accuracy": _ratio(correct_count, scored_ground_truth_count),
        "micro_precision": micro_precision,
        "micro_recall": micro_recall,
        "micro_f1_score": _f1(micro_precision, micro_recall),
        "mean_absolute_error_seconds": _ratio(timing_error_sum, scored_matches),
        "max_absolute_error_seconds": max_absolute_error_seconds,
    }
    return {
        "success": True,
        "target": "ecg_beat_classification",
        "tolerance_seconds": tolerance_seconds,
        "summary": summary,
        "classes": [_class_row(name, classes[name]) for name in BEAT_CLASSES],
        "confusion_matrix": {"labels": list(BEAT_CLASSES), "rows": [[confusion[actual][predicted] for predicted in BEAT_CLASSES] for actual in BEAT_CLASSES]},
        "matches": matches,
        "unmatched_ground_truth": unmatched_ground_truth,
        "unmatched_predictions": unmatched_predictions,
    }


def _truth_events_for_target(target, annotations, case_summary):
    events = []
    if target == "r_peak":
        for array_index, beat in enumerate(annotations.get("beats", [])):
            if beat.get("qrs_present", False) and _finite_non_negative(beat.get("r_peak_seconds")):
                time_seconds = float(beat["r_peak_seconds"])
                index = beat.get("beat_index", array_index)
                events.append(_TruthEvent(
                    index, time_seconds, "", _in_artifact_interval(target, time_seconds, annotations, case_summary),
                    scoreable=beat.get("r_peak_scoreable", True),
                    exclusion_reason=beat.get("r_peak_exclusion_reason", "")))
        return events
    if target == "ecg_beat_classification":
        for array_index, beat in enumerate(annotations.get("beats", [])):
            if beat.get("qrs_present", False) and _finite_non_negative(beat.get("r_peak_seconds")):
                label = beat.get("beat_class", "unscored")
                if label not in BEAT_CLASSES:
                    label = "unscored"
                scoreable = bool(beat.get("r_peak_scoreable", True))
                if not scoreable:
                    label = "unscored"
                events.append(_TruthEvent(
                    beat.get("beat_index", array_index), beat["r_peak_seconds"], label, False,
                    scoreable=scoreable, exclusion_reason=beat.get("r_peak_exclusion_reason", "")))
        return events
    if target in PPG_EVENT_TARGETS:
        expected_kind = "systolic_peak" if target == "ppg_systolic_peak" else "pulse_onset"
        for item in annotations.get("ppg_fiducials", []):
            if item.get("kind") == expected_kind and item.get("source") == "measurement" and _finite_non_negative(item.get("time_seconds")):
                time_seconds = float(item["time_seconds"])
                pulse = _ppg_pulse_for_beat(annotations, item.get("ecg_beat_index"))
                scoreable = bool(item.get("scoreable", True)) and bool(
                    pulse is None or pulse.get("valid_for_peak_scoring", True))
                events.append(_TruthEvent(
                    len(events), time_seconds, "", _in_artifact_interval(target, time_seconds, annotations, case_summary),
                    _in_motion_artifact_interval(target, time_seconds, annotations, case_summary),
                    _in_dropout_artifact_interval(target, time_seconds, annotations, case_summary),
                    bool(pulse and pulse.get("low_perfusion", False)),
                    bool(pulse and _ppg_pulse_state(pulse) == "weak"),
                    scoreable=scoreable, exclusion_reason=item.get("exclusion_reason", "")))
        return events
    raise VerificationError("unsupported target: %s" % target)


def _ppg_pulse_for_beat(annotations, beat_index):
    for pulse in annotations.get("ppg_pulses", []):
        if pulse.get("ecg_beat_index") == beat_index:
            return pulse
    return None


def _ppg_pulse_state(pulse):
    if pulse.get("state"):
        return pulse["state"]
    return "missing" if pulse.get("intentionally_missing", False) else "valid"


def _ppg_pulse_at_time(annotations, time_seconds):
    best = None
    best_distance = None
    for pulse in annotations.get("ppg_pulses", []):
        onset = float(pulse.get("expected_onset_time_seconds", 0.0))
        offset = float(pulse.get("expected_offset_time_seconds", onset))
        if time_seconds < onset or time_seconds > offset:
            continue
        distance = abs(time_seconds - float(pulse.get("expected_peak_time_seconds", onset)))
        if best is None or distance < best_distance:
            best = pulse
            best_distance = distance
    return best


def _in_artifact_interval(target, time_seconds, annotations, case_summary):
    intervals = annotations.get("artifact_intervals")
    if intervals is None:
        intervals = case_summary.get("artifact_intervals", [])
    for interval in intervals:
        start = float(interval.get("start_seconds", 0.0))
        end = float(interval.get("end_seconds", 0.0))
        if time_seconds >= start and time_seconds < end and _artifact_affects_target(interval, target):
            return True
    return False


def _in_motion_artifact_interval(target, time_seconds, annotations, case_summary):
    if target not in PPG_EVENT_TARGETS:
        return False
    intervals = annotations.get("artifact_intervals")
    if intervals is None:
        intervals = case_summary.get("artifact_intervals", [])
    for interval in intervals:
        artifact_type = str(interval.get("type", ""))
        start = float(interval.get("start_seconds", 0.0))
        end = float(interval.get("end_seconds", 0.0))
        if artifact_type.startswith("ppg_motion_") and time_seconds >= start and time_seconds < end:
            return True
    return False


def _in_dropout_artifact_interval(target, time_seconds, annotations, case_summary):
    if target not in PPG_EVENT_TARGETS:
        return False
    intervals = annotations.get("artifact_intervals")
    if intervals is None:
        intervals = case_summary.get("artifact_intervals", [])
    for interval in intervals:
        start = float(interval.get("start_seconds", 0.0))
        end = float(interval.get("end_seconds", 0.0))
        if interval.get("type") == "ppg_dropout" and time_seconds >= start and time_seconds < end:
            return True
    return False


def _artifact_affects_target(interval, target):
    channels = [str(item).lower() for item in interval.get("channels", [])]
    if not channels:
        return True
    if target in PPG_EVENT_TARGETS:
        return any(item == "ppg_green" or "ppg" in item for item in channels)
    return any("ppg" not in item for item in channels)


def _case_scoring_entries(scoring_manifest, case_id, case_summary):
    for item in scoring_manifest.get("cases", []):
        if item.get("case_id") == case_id:
            return list(item.get("scoring", []))
    return list(case_summary.get("scoring", []))


def _detection_stem_for_target(case_id, case_targets, target):
    return case_id if len(case_targets) <= 1 else case_id + "_" + target


def _target_output_relative_path(case_id, case_targets, target):
    return _safe_relative_path("details/" + _detection_stem_for_target(case_id, case_targets, target) + ".html")


def _safe_relative_path(path):
    normalized = path.replace("\\", "/")
    if os.path.isabs(normalized) or ".." in normalized.split("/"):
        raise VerificationError("unsafe relative output path: %s" % path)
    return normalized


def _case_result_from_report(case, case_summary, target, relative_out, submission_path, submission_identity, report_json):
    result = {
        "case_id": case.id,
        "scenario_id": case_summary.get("scenario_id", case.scenario_id),
        "target": target,
        "status": "scored",
        "success": True,
        "report_path": relative_out,
        "submission_output": submission_identity,
        "submission_output_sha256": _sha256_file(submission_path),
        "_report": report_json,
    }
    if target == "ecg_beat_classification":
        result["score_type"] = "classification"
        result["exclusion_policy"] = "Ground-truth beats explicitly marked unscoreable, or labelled unscored, are paired for traceability but excluded from accuracy, recall, and F1 denominators."
        result["summary"] = dict(report_json.get("summary", {}))
        result["classes"] = list(report_json.get("classes", []))
        result["confusion_matrix"] = dict(report_json.get("confusion_matrix", {}))
    elif target in INTERVAL_TARGETS:
        result["score_type"] = "interval_detection"
        result["exclusion_policy"] = "Half-open intervals are scored by label and channel; global and per-channel signal-quality modes are not mixed."
        result["summary"] = dict(report_json.get("overall", {}))
        result["classes"] = list(report_json.get("classes", []))
        result["confusion_matrix"] = list(report_json.get("confusion_matrix", []))
        result["_interval_onset_errors"] = [float(item["onset_error_seconds"]) for item in report_json.get("matches", [])]
        result["_interval_offset_errors"] = [float(item["offset_error_seconds"]) for item in report_json.get("matches", [])]
        result["record_duration_seconds"] = float(report_json.get("scenario", {}).get("duration_seconds", 0.0))
    elif target == "ecg_delineation":
        result["score_type"] = "ecg_delineation"
        result["exclusion_policy"] = "Present truth is scored, predictions for absent waves are false positives, and predictions inside not-evaluable wave windows are excluded."
        result["summary"] = dict(report_json.get("overall", {}))
        result["by_kind"] = list(report_json.get("by_kind", []))
        result["by_lead"] = list(report_json.get("by_lead", []))
        result["_delineation_errors"] = [float(item["error_seconds"]) for item in report_json.get("matches", [])]
    elif target in MEASUREMENT_TARGETS:
        result["score_type"] = "measurement"
        result["exclusion_policy"] = "Measurement identity includes name, unit, scope, channel, formula, method, preprocessing policy, window, and beat/time anchor; explicit non-valid states are status-scored without numeric error."
        result["summary"] = dict(report_json.get("overall", {}))
        result["by_measurement"] = list(report_json.get("by_measurement", []))
        result["by_channel"] = list(report_json.get("by_channel", []))
        result["by_measurement_context"] = list(report_json.get("by_measurement_context", []))
        result["_measurement_errors"] = [_measurement_error_record(item) for item in report_json.get("matches", []) if item.get("numeric_pair", False)]
    else:
        result["score_type"] = "event_detection"
        result["exclusion_policy"] = "Observable events are scored; explicitly reasoned near-total-dropout and truncated-boundary truth is excluded. A nearby otherwise-unmatched prediction is reported and omitted from FP counts. All other artifact and severe-noise events remain scored."
        result["metrics"] = dict(report_json.get("comparison", {}).get("metrics", {}))
        errors = dict((name, []) for name in ("total", "clean", "artifact", "motion", "dropout", "low_perfusion"))
        for match in report_json.get("comparison", {}).get("matches", []):
            value = abs(float(match["error_seconds"]))
            errors["total"].append(value)
            errors["artifact" if match.get("in_artifact_interval", False) else "clean"].append(value)
            if match.get("in_motion_artifact_interval", False):
                errors["motion"].append(value)
            if match.get("in_dropout_artifact_interval", False):
                errors["dropout"].append(value)
            if match.get("low_perfusion", False):
                errors["low_perfusion"].append(value)
        result["_absolute_errors"] = errors
    return result


def _unsupported_result(case, case_summary, entry):
    target = entry.get("target", "")
    message = entry.get("note", "target is not supported by local verifier")
    result = {
        "case_id": case.id,
        "scenario_id": case_summary.get("scenario_id", case.scenario_id),
        "target": target,
        "score_type": entry.get("score_type", ""),
        "status": "unsupported",
        "success": False,
        "message": message,
        "report_path": _target_output_relative_path(case.id, case_summary.get("targets", []), target),
    }
    result["_report"] = {
        "schema_version": 1,
        "score_type": result["score_type"],
        "scoring_version": SCORING_VERSION,
        "scenario": _scenario_identity(case, case_summary, {}),
        "target": target,
        "success": False,
        "status": "unsupported",
        "message": message,
        "notes": [LIMITATION_TEXT],
    }
    return result


def _error_result(package, case, case_summary, entry, detail_path, status, message, detection_path=None):
    target = entry.get("target", "")
    report = {
        "schema_version": 1,
        "score_type": entry.get("score_type", ""),
        "scoring_version": SCORING_VERSION,
        "package": _package_identity(package),
        "scenario": _scenario_identity(case, case_summary, {}),
        "target": target,
        "success": False,
        "status": status,
        "message": message,
        "notes": [LIMITATION_TEXT],
    }
    if detection_path is not None:
        report["submission_output_path"] = detection_path
        report["submission_output_sha256"] = _sha256_file(detection_path)
    return {
        "case_id": case.id,
        "scenario_id": case_summary.get("scenario_id", case.scenario_id),
        "target": target,
        "score_type": entry.get("score_type", ""),
        "status": status,
        "success": False,
        "message": message,
        "report_path": detail_path,
        "_report": report,
    }


def _build_evidence_record(package, scoring_manifest, submission, integrity, results, messages, threshold_profile, acceptance_strata, verification):
    targets = _aggregate_targets(results)
    hrv_pipeline = _build_hrv_pipeline_diagnostics(targets)
    scoring_success = bool(results) and all(item.get("success", False) for item in results)
    policy = _evaluate_policy(targets, threshold_profile)
    policy = annotate_policy(_apply_acceptance_strata(targets, results, policy, acceptance_strata))
    _annotate_policy_contributions(policy, results)
    success = scoring_success and policy["passed"]
    executed_matrix = set((item.get("case_id", ""), item.get("target", "")) for item in results)
    required_matrix = verification.pop("required_matrix")
    executed_required_matrix = executed_matrix & required_matrix
    required_results = [item for item in results if (item.get("case_id", ""), item.get("target", "")) in required_matrix]
    incomplete_statuses = set(["missing_submission_entry", "missing_output", "unsupported"])
    matrix_complete = bool(required_matrix) and executed_required_matrix == required_matrix and not any(item.get("status") in incomplete_statuses for item in required_results)
    evidence_eligible = verification["mode"] == "evidence" and matrix_complete and verification.get("protocol") is not None
    verification["matrix_complete"] = matrix_complete
    verification["evidence_eligible"] = evidence_eligible
    verification["required_case_target_count"] = len(required_matrix)
    verification["executed_case_target_count"] = len(executed_required_matrix)
    verification["evidence_passed"] = evidence_eligible and success
    evidence_results = []
    for result in results:
        evidence_result = {
            key: result[key]
            for key in (
                "case_id", "scenario_id", "target", "score_type", "status",
                "success", "message", "report_path", "submission_output",
                "submission_output_sha256", "exclusion_policy",
            )
            if key in result
        }
        evidence_result["criterion_ids"] = [
            check["criterion_id"]
            for check in policy.get("checks", [])
            if any(
                contribution.get("case_id") == result.get("case_id")
                for contribution in check.get("case_contributions", [])
            )
        ]
        evidence_result["comparison"] = result.get("_report", {})
        evidence_results.append(evidence_result)
    return {
        "schema_version": 3,
        "contract": "synsigra_local_verification_v3",
        "record_type": "synsigra_verification_evidence",
        "scoring_version": SCORING_VERSION,
        "status": "%s_%s" % (verification["mode"], "passed" if success else "failed"),
        "success": success,
        "scoring_success": scoring_success,
        "threshold_profile": threshold_profile,
        "verification": verification,
        "policy": policy,
        "package": _package_identity(package, scoring_manifest),
        "submission": {"contract": "synsigra_submission_v1", "challenge": dict(submission.challenge), "algorithm": dict(submission.algorithm), "output_count": len(submission.outputs)},
        "integrity": integrity,
        "case_target_count": len(results),
        "completed_case_target_count": sum(1 for item in results if item.get("success", False)),
        "incomplete_case_target_count": sum(1 for item in results if not item.get("success", False)),
        "targets": targets,
        "hrv_pipeline": hrv_pipeline,
        "results": evidence_results,
        "messages": messages,
        "limitation": LIMITATION_TEXT,
    }


def _write_verification_bundle(out_dir, evidence):
    generated = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    evidence["generated_at_utc"] = generated.isoformat().replace("+00:00", "Z")
    results = evidence["results"]
    evidence["artifacts"] = {
        "entry_point": "index.html",
        "machine_evidence": "evidence.json",
        "detail_reports": [result.get("report_path", "") for result in results],
        "file_count": len(results) + 2,
    }
    evidence_path = os.path.join(out_dir, "evidence.json")
    index_path = os.path.join(out_dir, "index.html")
    _write_json(evidence_path, evidence)
    for result in results:
        detail_path = result.get("report_path", "")
        if not detail_path:
            continue
        _write_text(
            os.path.join(out_dir, detail_path),
            render_detail(evidence, result, result.get("comparison", {})),
        )
    _write_text(index_path, render_index(evidence))
    return evidence_path, index_path


def _build_hrv_pipeline_diagnostics(targets):
    by_name = dict((item.get("target", ""), item) for item in targets)
    hrv = by_name.get("hrv")
    if hrv is None:
        return {"available": False, "complete": False, "stages": []}

    r_peak = by_name.get("r_peak")
    quality = by_name.get("signal_quality")
    rr = hrv.get("rr", {})
    stages = [
        {
            "stage": "r_peak_detection",
            "target": "r_peak",
            "available": r_peak is not None,
            "score_name": "f1_score",
            "score": r_peak.get("total", {}).get("f1_score") if r_peak is not None else None,
        },
        {
            "stage": "rr_interval_reconstruction",
            "target": "hrv",
            "available": int(rr.get("evaluated_case_count", 0)) > 0,
            "score_name": "rr_pass_fraction",
            "score": rr.get("pass_fraction") if int(rr.get("evaluated_case_count", 0)) > 0 else None,
        },
        {
            "stage": "hrv_metric_computation",
            "target": "hrv",
            "available": int(hrv.get("evaluated_metric_count", 0)) > 0,
            "score_name": "metric_pass_fraction",
            "score": hrv.get("metric_pass_fraction") if int(hrv.get("evaluated_metric_count", 0)) > 0 else None,
        },
        {
            "stage": "signal_quality_interval_detection",
            "target": "signal_quality",
            "available": quality is not None,
            "score_name": "time_f1_score",
            "score": quality.get("overall", {}).get("time_f1_score") if quality is not None else None,
        },
    ]
    required = stages[:3]
    return {"available": True, "complete": all(item["available"] for item in required), "stages": stages}


def _aggregate_targets(results):
    aggregates = {}
    for result in results:
        target = result.get("target", "")
        if target not in aggregates:
            aggregates[target] = {"target": target, "case_count": 0, "passed_case_count": 0, "failed_case_count": 0}
        aggregate = aggregates[target]
        if result.get("exclusion_policy") and "exclusion_policy" not in aggregate:
            aggregate["exclusion_policy"] = result["exclusion_policy"]
        aggregate["case_count"] += 1
        if result.get("success", False):
            aggregate["passed_case_count"] += 1
        else:
            aggregate["failed_case_count"] += 1
        if result.get("score_type") == "event_detection" and result.get("success", False):
            _aggregate_event_result(aggregate, result)
        if result.get("score_type") == "classification" and result.get("success", False):
            _aggregate_classification_result(aggregate, result)
        if result.get("score_type") == "interval_detection" and result.get("success", False):
            _aggregate_interval_result(aggregate, result)
        if result.get("score_type") == "ecg_delineation" and result.get("success", False):
            _aggregate_delineation_result(aggregate, result)
        if result.get("score_type") == "measurement" and result.get("success", False):
            _aggregate_measurement_result(aggregate, result)
    ordered = []
    for target in sorted(aggregates.keys()):
        aggregate = aggregates[target]
        if "event_errors_total" in aggregate:
            for name in ("total", "clean", "artifact", "motion", "dropout", "low_perfusion"):
                _finalize_event_metrics(aggregate[name], aggregate.pop("event_errors_" + name))
        if "scored_ground_truth_count" in aggregate:
            aggregate["accuracy"] = _ratio(aggregate["correct_count"], aggregate["scored_ground_truth_count"])
            aggregate["micro_precision"] = _ratio(aggregate["correct_count"], aggregate["scored_prediction_count"])
            aggregate["micro_recall"] = _ratio(aggregate["correct_count"], aggregate["scored_ground_truth_count"])
            aggregate["micro_f1_score"] = _f1(aggregate["micro_precision"], aggregate["micro_recall"])
            _finalize_classification_aggregate(aggregate)
        if aggregate.get("score_type") == "interval_detection":
            _finalize_interval_aggregate(aggregate)
        if aggregate.get("score_type") == "ecg_delineation":
            _finalize_delineation_aggregate(aggregate)
        if aggregate.get("score_type") == "measurement":
            _finalize_measurement_aggregate(aggregate)
        ordered.append(aggregate)
    return ordered


def _aggregate_event_result(aggregate, result):
    if "total" not in aggregate:
        aggregate["score_type"] = "event_detection"
        for name in ("total", "clean", "artifact", "motion", "dropout", "low_perfusion"):
            aggregate[name] = _empty_event_metrics()
            aggregate["event_errors_" + name] = []
    metrics = result.get("metrics", {})
    for name in ("total", "clean", "artifact", "motion", "dropout", "low_perfusion"):
        source = metrics.get(name, {})
        for key in ("ground_truth_count", "detection_count", "true_positive_count", "false_positive_count", "false_negative_count", "excluded_ground_truth_count", "excluded_detection_count"):
            aggregate[name][key] += int(source.get(key, 0))
        aggregate["event_errors_" + name].extend(result.get("_absolute_errors", {}).get(name, []))


def _aggregate_classification_result(aggregate, result):
    if "score_type" not in aggregate:
        aggregate["score_type"] = "classification"
        aggregate["scored_ground_truth_count"] = 0
        aggregate["scored_prediction_count"] = 0
        aggregate["matched_count"] = 0
        aggregate["correct_count"] = 0
        aggregate["classes"] = dict((name, _empty_class_metrics(name != "unscored")) for name in BEAT_CLASSES)
        aggregate["confusion_matrix"] = dict((actual, dict((predicted, 0) for predicted in BEAT_CLASSES)) for actual in BEAT_CLASSES)
    summary = result.get("summary", {})
    for key in ("scored_ground_truth_count", "scored_prediction_count", "matched_count", "correct_count"):
        aggregate[key] += int(summary.get(key, 0))
    for item in result.get("classes", []):
        name = item.get("class")
        if name not in aggregate["classes"]:
            continue
        for key in ("ground_truth_count", "prediction_count", "true_positive_count", "false_positive_count", "false_negative_count"):
            aggregate["classes"][name][key] += int(item.get(key, 0))
    matrix = result.get("confusion_matrix", {})
    labels = matrix.get("labels", [])
    for row_index, actual in enumerate(labels):
        if actual not in aggregate["confusion_matrix"] or row_index >= len(matrix.get("rows", [])):
            continue
        for column_index, predicted in enumerate(labels):
            if predicted in aggregate["confusion_matrix"][actual] and column_index < len(matrix["rows"][row_index]):
                aggregate["confusion_matrix"][actual][predicted] += int(matrix["rows"][row_index][column_index])


def _finalize_classification_aggregate(aggregate):
    class_rows = []
    for name in BEAT_CLASSES:
        metrics = aggregate["classes"][name]
        if metrics["scored"]:
            metrics["precision"] = _ratio(metrics["true_positive_count"], metrics["true_positive_count"] + metrics["false_positive_count"])
            metrics["recall"] = _ratio(metrics["true_positive_count"], metrics["true_positive_count"] + metrics["false_negative_count"])
            metrics["f1_score"] = _f1(metrics["precision"], metrics["recall"])
        class_rows.append(_class_row(name, metrics))
    aggregate["classes"] = class_rows
    confusion = aggregate["confusion_matrix"]
    aggregate["confusion_matrix"] = {"labels": list(BEAT_CLASSES), "rows": [[confusion[actual][predicted] for predicted in BEAT_CLASSES] for actual in BEAT_CLASSES]}


def _aggregate_interval_result(aggregate, result):
    if aggregate.get("score_type") != "interval_detection":
        aggregate["score_type"] = "interval_detection"
        aggregate["overall"] = _empty_interval_aggregate_metrics()
        aggregate["classes"] = {}
        aggregate["confusion_matrix"] = {}
        aggregate["_record_duration_seconds"] = 0.0
        aggregate["_onset_errors"] = []
        aggregate["_offset_errors"] = []
    _accumulate_interval_metrics(aggregate["overall"], result.get("summary", {}))
    aggregate["_record_duration_seconds"] += float(result.get("record_duration_seconds", 0.0))
    aggregate["_onset_errors"].extend(result.get("_interval_onset_errors", []))
    aggregate["_offset_errors"].extend(result.get("_interval_offset_errors", []))
    for item in result.get("classes", []):
        label = item.get("label", "")
        row = aggregate["classes"].setdefault(label, _empty_interval_aggregate_metrics())
        _accumulate_interval_metrics(row, item.get("metrics", {}))
    for item in result.get("confusion_matrix", []):
        key = (item.get("ground_truth_label", ""), item.get("prediction_label", ""))
        aggregate["confusion_matrix"][key] = aggregate["confusion_matrix"].get(key, 0) + int(item.get("count", 0))


def _finalize_interval_aggregate(aggregate):
    duration = aggregate.pop("_record_duration_seconds")
    onset_errors = aggregate.pop("_onset_errors")
    offset_errors = aggregate.pop("_offset_errors")
    _finalize_interval_aggregate_metrics(aggregate["overall"], duration, onset_errors, offset_errors)
    class_rows = []
    for label in sorted(aggregate["classes"].keys()):
        metrics = aggregate["classes"][label]
        _finalize_interval_aggregate_metrics(metrics, duration, [], [])
        class_rows.append({"label": label, "metrics": metrics})
    aggregate["classes"] = class_rows
    confusion = aggregate["confusion_matrix"]
    aggregate["confusion_matrix"] = [{"ground_truth_label": key[0], "prediction_label": key[1], "count": confusion[key]} for key in sorted(confusion.keys())]


def _empty_interval_aggregate_metrics():
    return {
        "ground_truth_count": 0, "prediction_count": 0, "matched_count": 0, "false_alarm_count": 0, "missed_count": 0,
        "ground_truth_duration_seconds": 0.0, "prediction_duration_seconds": 0.0, "overlap_duration_seconds": 0.0,
    }


def _accumulate_interval_metrics(destination, source):
    for key in ("ground_truth_count", "prediction_count", "matched_count", "false_alarm_count", "missed_count"):
        destination[key] += int(source.get(key, 0))
    for key in ("ground_truth_duration_seconds", "prediction_duration_seconds", "overlap_duration_seconds"):
        destination[key] += float(source.get(key, 0.0))


def _finalize_interval_aggregate_metrics(metrics, duration, onset_errors, offset_errors):
    truth_duration = metrics["ground_truth_duration_seconds"]
    prediction_duration = metrics["prediction_duration_seconds"]
    overlap = metrics["overlap_duration_seconds"]
    metrics["time_sensitivity"] = overlap / truth_duration if truth_duration > 0.0 else None
    metrics["time_precision"] = overlap / prediction_duration if prediction_duration > 0.0 else None
    metrics["time_f1_score"] = 2.0 * overlap / (truth_duration + prediction_duration) if truth_duration + prediction_duration > 0.0 else None
    union = truth_duration + prediction_duration - overlap
    metrics["temporal_iou"] = overlap / union if union > 0.0 else None
    metrics["event_sensitivity"] = float(metrics["matched_count"]) / metrics["ground_truth_count"] if metrics["ground_truth_count"] else None
    metrics["event_precision"] = float(metrics["matched_count"]) / metrics["prediction_count"] if metrics["prediction_count"] else None
    metrics["false_alarms_per_hour"] = metrics["false_alarm_count"] * 3600.0 / duration if duration > 0.0 else 0.0
    metrics["mean_absolute_onset_error_seconds"] = _mean([abs(value) for value in onset_errors]) if onset_errors else None
    metrics["mean_absolute_offset_error_seconds"] = _mean([abs(value) for value in offset_errors]) if offset_errors else None


def _aggregate_delineation_result(aggregate, result):
    if aggregate.get("score_type") != "ecg_delineation":
        aggregate["score_type"] = "ecg_delineation"
        aggregate["overall"] = dict((key, 0) for key in ("ground_truth_count", "prediction_count", "paired_count", "within_tolerance_count", "missing_prediction_count", "unexpected_prediction_count", "out_of_tolerance_count", "false_negative_count", "false_positive_count"))
        aggregate["_errors"] = []
    source = result.get("summary", {})
    for key in aggregate["overall"]:
        aggregate["overall"][key] += int(source.get(key, 0))
    aggregate["_errors"].extend(result.get("_delineation_errors", []))


def _finalize_delineation_aggregate(aggregate):
    metrics = aggregate["overall"]
    errors = aggregate.pop("_errors")
    truth = metrics["ground_truth_count"]
    predicted = metrics["prediction_count"]
    within = metrics["within_tolerance_count"]
    paired = metrics["paired_count"]
    metrics["sensitivity"] = float(within) / truth if truth else None
    metrics["positive_predictive_value"] = float(within) / predicted if predicted else None
    metrics["f1_score"] = 2.0 * within / (truth + predicted) if truth + predicted else None
    metrics["within_tolerance_fraction"] = float(within) / paired if paired else None
    absolute = sorted(abs(value) for value in errors)
    metrics["mean_error_seconds"] = _mean(errors) if errors else None
    metrics["mean_absolute_error_seconds"] = _mean(absolute) if absolute else None
    metrics["p95_absolute_error_seconds"] = absolute[int(math.ceil(0.95 * len(absolute))) - 1] if absolute else None


def _aggregate_measurement_result(aggregate, result):
    if aggregate.get("score_type") != "measurement":
        aggregate["score_type"] = "measurement"
        aggregate["overall"] = _empty_measurement_metrics()
        aggregate["by_measurement"] = {}
        aggregate["by_channel"] = {}
        aggregate["by_measurement_context"] = {}
        aggregate["_errors"] = []
        aggregate["_errors_by_measurement"] = {}
        aggregate["_errors_by_channel"] = {}
        aggregate["_errors_by_context"] = {}
    _accumulate_measurement_metrics(aggregate["overall"], result.get("summary", {}))
    for item in result.get("by_measurement", []):
        name = item.get("name", "")
        if name not in aggregate["by_measurement"]:
            aggregate["by_measurement"][name] = _empty_measurement_metrics()
        _accumulate_measurement_metrics(aggregate["by_measurement"][name], item.get("metrics", {}))
    for item in result.get("by_channel", []):
        channel = item.get("channel", "")
        if channel not in aggregate["by_channel"]:
            aggregate["by_channel"][channel] = _empty_measurement_metrics()
        _accumulate_measurement_metrics(aggregate["by_channel"][channel], item.get("metrics", {}))
    for item in result.get("by_measurement_context", []):
        key = _measurement_context_key(item)
        if key not in aggregate["by_measurement_context"]:
            aggregate["by_measurement_context"][key] = {"descriptor": dict((name, item[name]) for name in ("name", "unit", "scope", "channel", "formula", "method_id", "preprocessing_policy_id") if name in item), "metrics": _empty_measurement_metrics()}
            if "window_start_seconds" in item:
                aggregate["by_measurement_context"][key]["descriptor"]["window_start_seconds"] = item["window_start_seconds"]
                aggregate["by_measurement_context"][key]["descriptor"]["window_end_seconds"] = item["window_end_seconds"]
        _accumulate_measurement_metrics(aggregate["by_measurement_context"][key]["metrics"], item.get("metrics", {}))
    for item in result.get("_measurement_errors", []):
        error = float(item["error"])
        aggregate["_errors"].append(error)
        aggregate["_errors_by_measurement"].setdefault(item["name"], []).append(error)
        aggregate["_errors_by_channel"].setdefault(item["channel"], []).append(error)
        aggregate["_errors_by_context"].setdefault(_measurement_context_key(item), []).append(error)


def _finalize_measurement_aggregate(aggregate):
    _finalize_measurement_metrics(aggregate["overall"], aggregate.pop("_errors"))
    measurement_errors = aggregate.pop("_errors_by_measurement")
    channel_errors = aggregate.pop("_errors_by_channel")
    context_errors = aggregate.pop("_errors_by_context")
    measurement_rows = []
    for name in sorted(aggregate["by_measurement"]):
        metrics = aggregate["by_measurement"][name]
        _finalize_measurement_metrics(metrics, measurement_errors.get(name, []))
        measurement_rows.append({"name": name, "metrics": metrics})
    channel_rows = []
    for channel in sorted(aggregate["by_channel"]):
        metrics = aggregate["by_channel"][channel]
        _finalize_measurement_metrics(metrics, channel_errors.get(channel, []))
        channel_rows.append({"channel": channel, "metrics": metrics})
    aggregate["by_measurement"] = measurement_rows
    aggregate["by_channel"] = channel_rows
    context_rows = []
    for key in sorted(aggregate["by_measurement_context"], key=lambda value: tuple("" if item is None else str(item) for item in value)):
        row = aggregate["by_measurement_context"][key]
        _finalize_measurement_metrics(row["metrics"], context_errors.get(key, []))
        descriptor = dict(row["descriptor"])
        descriptor["metrics"] = row["metrics"]
        context_rows.append(descriptor)
    aggregate["by_measurement_context"] = context_rows
    if aggregate.get("target") == "hrv":
        by_name = dict((item["name"], item["metrics"]) for item in measurement_rows)
        rr = by_name.get("rr_interval", {})
        metric_rows = [item["metrics"] for item in measurement_rows if item["name"] != "rr_interval"]
        metric_numeric_count = sum(int(item.get("numeric_pair_count", 0)) for item in metric_rows)
        metric_pass_count = sum(int(item.get("tolerance_pass_count", 0)) for item in metric_rows)
        aggregate["evaluated_metric_count"] = metric_numeric_count
        aggregate["passed_metric_count"] = metric_pass_count
        aggregate["metric_pass_fraction"] = float(metric_pass_count) / metric_numeric_count if metric_numeric_count else None
        aggregate["rr"] = {
            "evaluated_case_count": aggregate.get("passed_case_count", 0),
            "ground_truth_count": int(rr.get("ground_truth_count", 0)),
            "prediction_count": int(rr.get("prediction_count", 0)),
            "matched_count": int(rr.get("matched_count", 0)),
            "missing_count": int(rr.get("missing_count", 0)),
            "extra_count": int(rr.get("extra_count", 0)),
            "pass_fraction": rr.get("tolerance_pass_fraction"),
            "mean_absolute_error_seconds": rr.get("error", {}).get("mean_absolute"),
        }


def _measurement_context_key(item):
    return tuple(item.get(name) for name in ("name", "unit", "scope", "channel", "formula", "method_id", "preprocessing_policy_id", "window_start_seconds", "window_end_seconds"))


def _measurement_error_record(item):
    output = dict((name, item.get(name)) for name in ("name", "unit", "scope", "channel", "formula", "method_id", "preprocessing_policy_id", "window_start_seconds", "window_end_seconds"))
    output["error"] = float(item["signed_error"])
    return output


def _empty_measurement_metrics():
    names = ("ground_truth_count", "valid_truth_count", "undefined_truth_count", "absent_truth_count", "not_evaluable_truth_count", "prediction_count", "matched_count", "covered_truth_count", "matched_prediction_count", "numeric_pair_count", "tolerance_pass_count", "status_match_count", "status_mismatch_count", "missing_count", "extra_count", "assertion_comparable_count", "assertion_agreement_count")
    output = dict((name, 0) for name in names)
    output.update({"tolerance_pass_fraction": None, "status_match_fraction": None, "assertion_agreement_fraction": None, "truth_match_fraction": None, "prediction_match_fraction": None, "error": _empty_measurement_error()})
    return output


def _empty_measurement_error():
    return dict((name, None) for name in ("bias", "mean_absolute", "root_mean_square", "median_absolute", "p95_absolute", "maximum_absolute"))


def _accumulate_measurement_metrics(destination, source):
    for name in ("ground_truth_count", "valid_truth_count", "undefined_truth_count", "absent_truth_count", "not_evaluable_truth_count", "prediction_count", "matched_count", "covered_truth_count", "matched_prediction_count", "numeric_pair_count", "tolerance_pass_count", "status_match_count", "status_mismatch_count", "missing_count", "extra_count", "assertion_comparable_count", "assertion_agreement_count"):
        destination[name] += int(source.get(name, 0))


def _finalize_measurement_metrics(metrics, errors):
    metrics["tolerance_pass_fraction"] = float(metrics["tolerance_pass_count"]) / metrics["numeric_pair_count"] if metrics["numeric_pair_count"] else None
    metrics["status_match_fraction"] = float(metrics["status_match_count"]) / metrics["matched_count"] if metrics["matched_count"] else None
    metrics["assertion_agreement_fraction"] = float(metrics["assertion_agreement_count"]) / metrics["assertion_comparable_count"] if metrics["assertion_comparable_count"] else None
    _covered = metrics.get("covered_truth_count", metrics["matched_count"])
    _matched_pred = metrics.get("matched_prediction_count", metrics["matched_count"])
    metrics["truth_match_fraction"] = float(_covered) / metrics["ground_truth_count"] if metrics["ground_truth_count"] else None
    metrics["prediction_match_fraction"] = float(_matched_pred) / metrics["prediction_count"] if metrics["prediction_count"] else None
    absolute = sorted(abs(item) for item in errors)
    metrics["error"] = _empty_measurement_error()
    if absolute:
        middle = len(absolute) // 2
        metrics["error"] = {"bias": _mean(errors), "mean_absolute": _mean(absolute), "root_mean_square": _rms(absolute), "median_absolute": absolute[middle] if len(absolute) & 1 else 0.5 * (absolute[middle - 1] + absolute[middle]), "p95_absolute": absolute[int(math.ceil(0.95 * len(absolute))) - 1], "maximum_absolute": absolute[-1]}


def _evaluate_policy(targets, profile):
    checks = []
    target_results = []
    definitions = profile.get("targets", {})
    for target in targets:
        score_type = target.get("score_type", "")
        definition_name = target["target"] if target["target"] in definitions else score_type
        if score_type == "classification":
            definition_name = "ecg_beat_classification"
        elif score_type == "interval_detection" and definition_name not in definitions:
            definition_name = "interval_detection"
        elif score_type == "ecg_delineation" and definition_name not in definitions:
            definition_name = "ecg_delineation"
        elif score_type == "measurement" and definition_name not in definitions:
            definition_name = "measurement"
        definition = definitions.get(definition_name, {})
        target_checks = []
        if score_type == "event_detection":
            for section_name, limits in definition.items():
                metrics = target.get(section_name, {})
                applicable = int(metrics.get("ground_truth_count", 0)) > 0 or int(metrics.get("detection_count", 0)) > 0
                target_checks.extend(_threshold_checks(target["target"], section_name, metrics, limits, applicable))
        elif score_type == "classification":
            target_checks.extend(_threshold_checks(target["target"], "summary", target, definition.get("summary", {}), target.get("scored_ground_truth_count", 0) > 0))
            per_class = definition.get("per_class", {})
            for class_metrics in target.get("classes", []):
                applicable = class_metrics.get("scored", False) and class_metrics.get("ground_truth_count", 0) > 0
                target_checks.extend(_threshold_checks(target["target"], "class/%s" % class_metrics.get("class", ""), class_metrics, per_class, applicable))
        elif score_type == "interval_detection":
            overall = target.get("overall", {})
            applicable = int(overall.get("ground_truth_count", 0)) > 0 or int(overall.get("prediction_count", 0)) > 0
            target_checks.extend(_threshold_checks(target["target"], "overall", overall, definition.get("overall", {}), applicable))
        elif score_type == "ecg_delineation":
            overall = target.get("overall", {})
            applicable = int(overall.get("ground_truth_count", 0)) > 0 or int(overall.get("prediction_count", 0)) > 0
            target_checks.extend(_threshold_checks(target["target"], "overall", overall, definition.get("overall", {}), applicable))
        elif score_type == "measurement":
            overall = target.get("overall", {})
            applicable = int(overall.get("ground_truth_count", 0)) > 0 or int(overall.get("prediction_count", 0)) > 0
            target_checks.extend(_threshold_checks(target["target"], "overall", overall, definition.get("overall", {}), applicable))
            measurements = dict((item.get("name", ""), item.get("metrics", {})) for item in target.get("by_measurement", []))
            for section_name, limits in definition.items():
                if section_name == "overall":
                    continue
                metrics = _measurement_policy_metrics(measurements.get(section_name, {}))
                applicable = int(metrics.get("ground_truth_count", 0)) > 0 or int(metrics.get("prediction_count", 0)) > 0
                target_checks.extend(_threshold_checks(target["target"], section_name, metrics, limits, applicable))
        target_passed = bool(target_checks) and all(item["passed"] for item in target_checks if item["applicable"])
        target["policy"] = {"profile_id": profile["profile_id"], "passed": target_passed, "checks": target_checks}
        target_results.append({"target": target["target"], "passed": target_passed, "check_count": len(target_checks)})
        checks.extend(target_checks)
    applicable_checks = [item for item in checks if item["applicable"]]
    passed = bool(target_results) and all(item["passed"] for item in target_results)
    return {
        "profile_id": profile["profile_id"],
        "passed": passed,
        "target_count": len(target_results),
        "check_count": len(checks),
        "applicable_check_count": len(applicable_checks),
        "failed_check_count": sum(1 for item in applicable_checks if not item["passed"]),
        "targets": target_results,
        "checks": checks,
    }


def _apply_acceptance_strata(targets, results, policy, acceptance_strata):
    if not acceptance_strata:
        policy["acceptance_strata"] = []
        return policy
    target_by_name = dict((target["target"], target) for target in targets)
    stratum_summaries = []
    for stratum in acceptance_strata:
        case_ids = set(stratum["case_ids"])
        profile = stratum["profile"]
        stratum_targets = [
            target for target in _aggregate_targets([
                result for result in results if result.get("case_id") in case_ids
            ])
            if target.get("target") in profile["targets"]
        ]
        stratum_policy = _evaluate_policy(stratum_targets, profile)
        for check in stratum_policy["checks"]:
            check["scope"] = "acceptance_stratum"
            check["stratum_id"] = stratum["id"]
            check["case_ids"] = list(stratum["case_ids"])
            target_by_name[check["target"]]["policy"]["checks"].append(check)
        stratum_summaries.append({
            "stratum_id": stratum["id"],
            "case_ids": list(stratum["case_ids"]),
            "profile_id": profile["profile_id"],
            "passed": stratum_policy["passed"],
            "check_count": stratum_policy["check_count"],
        })
        policy["checks"].extend(stratum_policy["checks"])

    target_results = []
    for target in targets:
        checks = target["policy"]["checks"]
        passed = bool(checks) and all(check["passed"] for check in checks if check["applicable"])
        target["policy"]["passed"] = passed
        target_results.append({"target": target["target"], "passed": passed, "check_count": len(checks)})
    applicable = [check for check in policy["checks"] if check["applicable"]]
    policy.update({
        "passed": bool(target_results) and all(target["passed"] for target in target_results),
        "targets": target_results,
        "check_count": len(policy["checks"]),
        "applicable_check_count": len(applicable),
        "failed_check_count": sum(1 for check in applicable if not check["passed"]),
        "acceptance_strata": stratum_summaries,
    })
    return policy


def _annotate_policy_contributions(policy, results):
    for check in policy.get("checks", []):
        scoped_cases = set(check.get("case_ids", []))
        contributions = []
        for result in results:
            if result.get("target") != check.get("target"):
                continue
            if scoped_cases and result.get("case_id") not in scoped_cases:
                continue
            metrics = _case_metrics_for_check(result, check)
            applicable = _case_metrics_applicable(result.get("score_type", ""), check.get("section", ""), metrics)
            value = metrics.get(check.get("metric")) if metrics is not None else None
            actual_present = (
                applicable and not isinstance(value, bool) and
                isinstance(value, (int, float)) and math.isfinite(float(value))
            )
            actual = float(value) if actual_present else None
            diagnostic_passed = None
            if actual_present:
                diagnostic_passed = (
                    actual >= float(check["threshold"])
                    if check.get("operator") == "min"
                    else actual <= float(check["threshold"])
                )
            contributions.append({
                "case_id": result.get("case_id", ""),
                "report_path": result.get("report_path", ""),
                "scoring_status": result.get("status", ""),
                "score_type": result.get("score_type", ""),
                "contributes": actual_present,
                "actual": actual,
                "diagnostic_passed": diagnostic_passed,
                "counts": _case_metric_counts(result.get("score_type", ""), metrics),
            })
        check["case_contributions"] = contributions
        check["contributing_case_count"] = sum(
            1 for item in contributions if item["contributes"]
        )


def _case_metrics_for_check(result, check):
    if not result.get("success", False):
        return None
    score_type = result.get("score_type", "")
    section = check.get("section", "")
    if score_type == "event_detection":
        return result.get("metrics", {}).get(section)
    if score_type == "classification":
        if section == "summary":
            return result.get("summary", {})
        if section.startswith("class/"):
            class_name = section.split("/", 1)[1]
            return next(
                (item for item in result.get("classes", []) if item.get("class") == class_name),
                None,
            )
    if score_type in ("interval_detection", "ecg_delineation"):
        return result.get("summary", {}) if section == "overall" else None
    if score_type == "measurement":
        if section == "overall":
            return _measurement_policy_metrics(result.get("summary", {}))
        item = next(
            (row for row in result.get("by_measurement", []) if row.get("name") == section),
            None,
        )
        return _measurement_policy_metrics(item.get("metrics", {})) if item else None
    return None


def _case_metrics_applicable(score_type, section, metrics):
    if metrics is None:
        return False
    if score_type == "event_detection":
        return int(metrics.get("ground_truth_count", 0)) > 0 or int(metrics.get("detection_count", 0)) > 0
    if score_type == "classification":
        if section == "summary":
            return int(metrics.get("scored_ground_truth_count", 0)) > 0
        return bool(metrics.get("scored", False)) and int(metrics.get("ground_truth_count", 0)) > 0
    return int(metrics.get("ground_truth_count", 0)) > 0 or int(metrics.get("prediction_count", 0)) > 0


def _case_metric_counts(score_type, metrics):
    if metrics is None:
        return {}
    if score_type == "event_detection":
        names = (
            "ground_truth_count", "detection_count", "true_positive_count",
            "false_positive_count", "false_negative_count",
            "excluded_ground_truth_count", "excluded_detection_count",
        )
    elif score_type == "classification":
        names = (
            "scored_ground_truth_count", "scored_prediction_count",
            "ground_truth_count", "prediction_count", "matched_count",
            "correct_count", "true_positive_count", "false_positive_count",
            "false_negative_count",
        )
    elif score_type in ("interval_detection", "ecg_delineation"):
        names = (
            "ground_truth_count", "prediction_count", "matched_count",
            "paired_count", "within_tolerance_count", "false_alarm_count",
            "missed_count", "false_positive_count", "false_negative_count",
        )
    else:
        names = (
            "ground_truth_count", "prediction_count", "matched_count",
            "numeric_pair_count", "tolerance_pass_count",
            "status_match_count", "status_mismatch_count",
            "missing_count", "extra_count",
        )
    return dict((name, int(metrics[name])) for name in names if name in metrics)


def _measurement_policy_metrics(metrics):
    output = dict(metrics)
    error = metrics.get("error", {})
    output.update({
        "mean_absolute_error": error.get("mean_absolute"),
        "root_mean_square_error": error.get("root_mean_square"),
        "p95_absolute_error": error.get("p95_absolute"),
        "maximum_absolute_error": error.get("maximum_absolute"),
    })
    return output


def _threshold_checks(target, section, metrics, limits, applicable):
    checks = []
    for metric_name in sorted(limits.keys()):
        value = metrics.get(metric_name)
        actual_present = metric_name in metrics and not isinstance(value, bool) and isinstance(value, (int, float))
        actual = float(value) if actual_present else 0.0
        for operator in ("min", "max"):
            if operator not in limits[metric_name]:
                continue
            threshold = float(limits[metric_name][operator])
            check_applicable = applicable and actual_present
            passed = not check_applicable or (actual >= threshold if operator == "min" else actual <= threshold)
            checks.append({
                "target": target,
                "section": section,
                "metric": metric_name,
                "operator": operator,
                "threshold": threshold,
                "actual": actual if actual_present else None,
                "applicable": check_applicable,
                "passed": passed,
            })
    return checks



def _empty_event_metrics():
    return {
        "ground_truth_count": 0,
        "detection_count": 0,
        "true_positive_count": 0,
        "false_positive_count": 0,
        "false_negative_count": 0,
        "excluded_ground_truth_count": 0,
        "excluded_detection_count": 0,
        "sensitivity": 0.0,
        "positive_predictive_value": 0.0,
        "f1_score": 0.0,
        "mean_absolute_error_seconds": 0.0,
        "median_absolute_error_seconds": 0.0,
        "rms_error_seconds": 0.0,
        "max_absolute_error_seconds": 0.0,
    }


def _empty_class_metrics(scored):
    return {
        "scored": scored,
        "ground_truth_count": 0,
        "prediction_count": 0,
        "true_positive_count": 0,
        "false_positive_count": 0,
        "false_negative_count": 0,
        "precision": 0.0,
        "recall": 0.0,
        "f1_score": 0.0,
    }


def _class_row(name, metrics):
    row = dict(metrics)
    row["class"] = name
    return row


def _finalize_event_metrics(metrics, absolute_errors):
    metrics["sensitivity"] = _ratio(metrics["true_positive_count"], metrics["ground_truth_count"])
    metrics["positive_predictive_value"] = _ratio(metrics["true_positive_count"], metrics["detection_count"])
    metrics["f1_score"] = _f1(metrics["sensitivity"], metrics["positive_predictive_value"])
    if absolute_errors:
        sorted_errors = sorted(absolute_errors)
        total = sum(sorted_errors)
        metrics["mean_absolute_error_seconds"] = total / len(sorted_errors)
        metrics["rms_error_seconds"] = math.sqrt(sum(item * item for item in sorted_errors) / len(sorted_errors))
        metrics["max_absolute_error_seconds"] = sorted_errors[-1]
        middle = len(sorted_errors) // 2
        if len(sorted_errors) & 1:
            metrics["median_absolute_error_seconds"] = sorted_errors[middle]
        else:
            metrics["median_absolute_error_seconds"] = 0.5 * (sorted_errors[middle - 1] + sorted_errors[middle])


def _bin_metrics(in_artifact_interval, clean, artifact):
    return artifact if in_artifact_interval else clean


def _ratio(numerator, denominator):
    return float(numerator) / float(denominator) if denominator else 0.0


def _f1(recall_or_precision, precision_or_recall):
    return 2.0 * recall_or_precision * precision_or_recall / (recall_or_precision + precision_or_recall) if recall_or_precision + precision_or_recall > 0.0 else 0.0


def _mean(values):
    return sum(values) / float(len(values)) if values else 0.0


def _rms(values):
    return math.sqrt(sum(item * item for item in values) / len(values)) if values else 0.0


def _default_tolerance_seconds(target):
    if target == "ppg_systolic_peak":
        return 0.080
    if target == "ecg_beat_classification":
        return 0.075
    return 0.050


def _finite_non_negative(value):
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        return False
    return numeric >= 0.0 and math.isfinite(numeric)


def _finite_positive(value):
    return _finite_non_negative(value) and float(value) > 0.0


def _submission_output_identity(path, relative_path, format_name, target, algorithm, count_name, count):
    output = {
        "path": relative_path,
        "sha256": _sha256_file(path),
        "target": target,
        "format": format_name,
        "algorithm": dict(algorithm),
    }
    output[count_name] = count
    return output


def _unique_json_object_pairs(pairs):
    output = {}
    for key, value in pairs:
        if key in output:
            raise VerificationError("duplicate JSON object key: %s" % key)
        output[key] = value
    return output


def _scenario_identity(case, case_summary, annotations):
    render = case_summary.get("render", {})
    identity = {
        "id": case_summary.get("scenario_id", case.scenario_id),
        "case_id": case.id,
        "document_fingerprint": case_summary.get("document_fingerprint", case.document_fingerprint),
        "render_identity": case_summary.get("render_identity", case.render_identity),
    }
    duration = render.get("duration_seconds", annotations.get("duration_seconds"))
    sample_rate = render.get("sample_rate_hz", annotations.get("sample_rate_hz"))
    if duration is not None:
        identity["duration_seconds"] = duration
    if sample_rate is not None:
        identity["sample_rate_hz"] = sample_rate
    channels = render.get("channels", case_summary.get("channels"))
    if isinstance(channels, list):
        identity["channel_count"] = len(channels)
        identity["channels"] = channels
    if "generation_fingerprint" in annotations:
        identity["generation_fingerprint"] = annotations["generation_fingerprint"]
    return identity


def _package_identity(package, scoring_manifest=None):
    identity = {
        "package_id": package.package_id,
        "name": package.name,
        "version": package.version,
        "package_type": package.manifest.get("package_type", ""),
        "generator_version": package.manifest.get("generator_version", ""),
        "ground_truth_included": package.manifest.get("ground_truth_included", False),
        "usage_restrictions": package.manifest.get("usage_restrictions", ""),
        "not_for": package.manifest.get("not_for", ""),
    }
    if scoring_manifest is not None:
        identity["pack_fingerprint"] = scoring_manifest.get("pack_fingerprint", "")
        identity["generator_git_commit"] = scoring_manifest.get("generator_git_commit", "")
        if not identity["generator_version"]:
            identity["generator_version"] = scoring_manifest.get("generator_version", "")
    return identity


def _normalize_filter(value):
    if value is None:
        return None
    if isinstance(value, str):
        return set([value])
    return set(value)


def _as_package(challenge):
    if isinstance(challenge, ChallengePackage):
        return challenge, False
    return load_challenge(challenge), True


def _prepare_output_dir(out_dir, force):
    if os.path.exists(out_dir):
        if not force:
            raise VerificationError("output directory already exists: %s" % out_dir)
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)


def _ensure_dir(path):
    if not os.path.isdir(path):
        os.makedirs(path)


def _write_json(path, data):
    with open(path, "w") as handle:
        json.dump(data, handle, sort_keys=True, indent=2)
        handle.write("\n")


def _write_text(path, text):
    parent = os.path.dirname(path)
    if parent:
        _ensure_dir(parent)
    with open(path, "w") as handle:
        handle.write(text)


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return "sha256:" + digest.hexdigest()
