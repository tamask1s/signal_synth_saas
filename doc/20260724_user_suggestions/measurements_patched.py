import csv
import html
import io
import json
import math
import os
import re


MEASUREMENT_FIELDS = ["name", "value", "unit", "status", "scope", "time_seconds", "beat_index", "window_start_seconds", "window_end_seconds", "channel", "formula", "method_id", "preprocessing_policy_id", "confidence"]
MEASUREMENT_STATUSES = set(["valid", "undefined", "absent", "not_evaluable"])
MEASUREMENT_SCOPES = set(["record", "lead", "beat", "beat_lead", "paired_signal", "window", "window_lead"])
MEASUREMENT_UNITS = set(["s", "s2", "mV", "mV/s", "deg", "count", "ratio", "nu", "%", "bpm", "a.u.", "bool"])
QT_FORMULAS = set(["fixed", "bazett", "fridericia", "framingham", "hodges"])
MEASUREMENT_TARGETS = frozenset(["rr_interval", "qtc", "hrv", "morphology_assertions", "ecg_ppg_alignment", "ppg_optical", "prv", "respiratory_rate", "rhythm_burden"])


class MeasurementError(ValueError):
    pass


def load_measurements(path, format_name=None):
    """Load and strictly validate a measurement_values_v2 user output."""
    selected = format_name or ("measurement_values_csv_v2" if str(path).lower().endswith(".csv") else "measurement_values_json_v2")
    if selected == "measurement_values_json_v2":
        with open(path, "r") as handle:
            raw = json.load(handle, object_pairs_hook=_unique_object)
        if not isinstance(raw, dict) or set(raw) != set(["schema_version", "contract", "measurements"]):
            raise MeasurementError("measurement JSON must contain exactly schema_version, contract, and measurements")
        if isinstance(raw["schema_version"], bool) or raw["schema_version"] != 2 or raw["contract"] != "synsigra_measurement_values_v2" or not isinstance(raw["measurements"], list):
            raise MeasurementError("measurement JSON requires the synsigra_measurement_values_v2 contract and a measurements array")
        records = raw["measurements"]
    elif selected == "measurement_values_csv_v2":
        try:
            with open(path, "r", newline="") as handle:
                reader = csv.reader(handle, strict=True)
                rows = list(reader)
        except csv.Error as error:
            raise MeasurementError("invalid measurement CSV: %s" % error)
        if not rows or rows[0] != MEASUREMENT_FIELDS:
            raise MeasurementError("measurement CSV header must exactly match the v2 column order")
        if any(len(row) != len(MEASUREMENT_FIELDS) for row in rows[1:]):
            raise MeasurementError("measurement CSV row has the wrong number of columns")
        records = [_csv_record(row, index) for index, row in enumerate(rows[1:])]
    else:
        raise MeasurementError("unsupported measurement format: %s" % selected)
    output = [_validate_measurement(item, index) for index, item in enumerate(records)]
    _require_unique(output)
    return output


def load_measurement_truth(path, target):
    """Load one target from a package-internal synsigra_measurement_truth_v2 artifact."""
    if target not in MEASUREMENT_TARGETS:
        raise MeasurementError("unsupported measurement target: %s" % target)
    with open(path, "r") as handle:
        raw = json.load(handle, object_pairs_hook=_unique_object)
    if not isinstance(raw, dict) or set(raw) != set(["schema_version", "contract", "targets"]):
        raise MeasurementError("measurement truth root has an invalid shape")
    if isinstance(raw["schema_version"], bool) or raw["schema_version"] != 2 or raw["contract"] != "synsigra_measurement_truth_v2" or not isinstance(raw["targets"], list):
        raise MeasurementError("measurement truth contract is not supported")
    target_map = {}
    for item in raw["targets"]:
        if not isinstance(item, dict) or set(item) != set(["target", "measurements"]) or item.get("target") not in MEASUREMENT_TARGETS or not isinstance(item.get("measurements"), list):
            raise MeasurementError("measurement truth contains an invalid target entry")
        if item["target"] in target_map:
            raise MeasurementError("measurement truth contains a duplicate target")
        target_map[item["target"]] = item
    selected = target_map.get(target)
    if selected is None:
        raise MeasurementError("measurement truth target must occur exactly once")
    output = []
    for index, item in enumerate(selected["measurements"]):
        allowed = set(["measurement", "absolute_tolerance", "relative_tolerance_percent", "error_model", "expected_range", "reason"])
        if not isinstance(item, dict) or not set(item).issubset(allowed) or not set(["measurement", "absolute_tolerance", "relative_tolerance_percent", "error_model", "reason"]).issubset(item):
            raise MeasurementError("measurement truth item %s has an invalid shape" % index)
        measurement = _validate_measurement(item["measurement"], index)
        absolute = _finite_number(item["absolute_tolerance"], "absolute_tolerance")
        relative = _finite_number(item["relative_tolerance_percent"], "relative_tolerance_percent")
        if absolute < 0.0 or relative < 0.0 or item["error_model"] not in ("linear", "circular_degrees") or not isinstance(item["reason"], str):
            raise MeasurementError("measurement truth item %s has invalid scoring metadata" % index)
        if item["error_model"] == "circular_degrees" and measurement["unit"] != "deg":
            raise MeasurementError("circular measurement truth requires degree units")
        truth = {"measurement": measurement, "absolute_tolerance": absolute, "relative_tolerance_percent": relative, "error_model": item["error_model"], "reason": item["reason"], "original_index": index}
        if "expected_range" in item:
            expected = item["expected_range"]
            if not isinstance(expected, dict) or set(expected) != set(["minimum", "maximum"]):
                raise MeasurementError("measurement truth expected_range is invalid")
            minimum = _finite_number(expected["minimum"], "expected_range.minimum")
            maximum = _finite_number(expected["maximum"], "expected_range.maximum")
            if minimum > maximum:
                raise MeasurementError("measurement truth expected_range is reversed")
            truth["expected_range"] = {"minimum": minimum, "maximum": maximum}
        output.append(truth)
    _require_unique([item["measurement"] for item in output])
    return output


def score_measurements(ground_truth, predictions, target, pairing_window_seconds=0.2):
    """Score measurement records using the C++ measurement_score_v2 policy."""
    if target not in MEASUREMENT_TARGETS:
        raise MeasurementError("unsupported measurement target: %s" % target)
    predictions = [_validate_measurement(dict(item), index) for index, item in enumerate(predictions)]
    _require_unique(predictions)
    pairing_window = _finite_number(pairing_window_seconds, "pairing_window_seconds")
    if pairing_window <= 0.0:
        raise MeasurementError("pairing_window_seconds must be positive")
    def _build_match(truth_index, prediction_index):
        truth_item = ground_truth[truth_index]
        truth = truth_item["measurement"]
        prediction = predictions[prediction_index]
        match = {
            "ground_truth_index": truth_index,
            "prediction_index": prediction_index,
            "name": truth["name"],
            "unit": truth["unit"],
            "scope": truth["scope"],
            "channel": truth.get("channel", "") or "global",
            "formula": truth.get("formula", ""),
            "method_id": truth.get("method_id", ""),
            "preprocessing_policy_id": truth.get("preprocessing_policy_id", ""),
            "ground_truth_status": truth["status"],
            "prediction_status": prediction["status"],
            "status_matches": truth["status"] == prediction["status"],
            "numeric_pair": truth["status"] == "valid" and prediction["status"] == "valid",
            "absolute_tolerance": truth_item["absolute_tolerance"],
            "relative_tolerance_percent": truth_item["relative_tolerance_percent"],
            "reason": truth_item["reason"],
        }
        if "window_start_seconds" in truth:
            match["window_start_seconds"] = truth["window_start_seconds"]
            match["window_end_seconds"] = truth["window_end_seconds"]
        if match["numeric_pair"]:
            error = prediction["value"] - truth["value"]
            if truth_item["error_model"] == "circular_degrees":
                error = (error + 180.0) % 360.0 - 180.0
            absolute_error = abs(error)
            relative_error = 100.0 * absolute_error / abs(truth["value"]) if abs(truth["value"]) > 1e-15 else None
            tolerance = truth_item["absolute_tolerance"]
            if relative_error is not None:
                tolerance = max(tolerance, abs(truth["value"]) * truth_item["relative_tolerance_percent"] / 100.0)
            match.update({
                "ground_truth_value": truth["value"],
                "prediction_value": prediction["value"],
                "signed_error": error,
                "absolute_error": absolute_error,
                "relative_error_percent": relative_error,
                "effective_tolerance": tolerance,
                "within_tolerance": absolute_error <= tolerance,
            })
            if "expected_range" in truth_item:
                expected = truth_item["expected_range"]
                match["ground_truth_assertion_passed"] = expected["minimum"] <= truth["value"] <= expected["maximum"]
                match["prediction_assertion_passed"] = expected["minimum"] <= prediction["value"] <= expected["maximum"]
        return match

    matches = []
    if target == "rr_interval":
        # Peak-anchored interval-overlap association (v0.14.3-overlap).
        # An RR interval spans [time_seconds - value, time_seconds] on the shared clock.
        # We associate every truth RR with every submitted RR whose interval interior
        # overlaps it. Because consecutive RR intervals meet exactly at a peak, the shared
        # (matched) peaks re-anchor the association at every beat:
        #   * a false-positive peak that splits one truth interval yields two overlapping
        #     submitted fragments, both compared to the same truth interval;
        #   * a false-negative peak that merges two truth intervals yields one submitted
        #     interval compared to both truth intervals;
        #   * N extra/missing peaks inside one truth span simply produce N+1 : 1 (or 1 : N+1)
        #     local comparisons.
        # This never shifts downstream beats and never collapses coverage. False-positive/
        # negative peaks contribute only bounded, local value errors (a fragment vs. the full
        # interval) instead of cascading a wrong pairing onto every subsequent beat. The
        # peak-level false-positive/negative accounting stays in the r_peak event metrics, so
        # RR value accuracy and R-peak detection are measured independently and are not
        # double-counted.
        truth_used = [False] * len(ground_truth)
        prediction_used = [False] * len(predictions)
        for truth_index, truth_item in enumerate(ground_truth):
            truth = truth_item["measurement"]
            if "time_seconds" not in truth or truth["status"] != "valid":
                continue
            truth_end = truth["time_seconds"]
            truth_start = truth_end - truth["value"]
            for prediction_index, prediction in enumerate(predictions):
                if _descriptor(truth) != _descriptor(prediction):
                    continue
                if "time_seconds" not in prediction or prediction["status"] != "valid":
                    continue
                prediction_end = prediction["time_seconds"]
                prediction_start = prediction_end - prediction["value"]
                overlap = min(truth_end, prediction_end) - max(truth_start, prediction_start)
                # Require a MAJORITY overlap of the shorter interval. Adjacent intervals only
                # touch at a shared peak, but detection jitter makes them overlap by a few ms;
                # a majority-overlap test rejects those spurious neighbours while still keeping
                # a fully-contained fragment (100% of its own length) or a merged interval.
                shorter = min(truth_end - truth_start, prediction_end - prediction_start)
                if shorter <= 0.0 or overlap <= 0.5 * shorter:
                    continue
                matches.append(_build_match(truth_index, prediction_index))
                truth_used[truth_index] = True
                prediction_used[prediction_index] = True
        missing = [index for index, used in enumerate(truth_used) if not used]
        extra = [index for index, used in enumerate(prediction_used) if not used]
    else:
        candidates = []
        for truth_index, truth_item in enumerate(ground_truth):
            truth = truth_item["measurement"]
            for prediction_index, prediction in enumerate(predictions):
                if _descriptor(truth) != _descriptor(prediction):
                    continue
                exact_beat = "beat_index" in truth and "beat_index" in prediction and truth["beat_index"] == prediction["beat_index"]
                if "beat_index" in truth and "beat_index" in prediction and not exact_beat:
                    continue
                if truth["scope"] in ("record", "lead", "window", "window_lead"):
                    distance = 0.0
                elif exact_beat:
                    distance = abs(truth.get("time_seconds", 0.0) - prediction.get("time_seconds", 0.0)) if "time_seconds" in truth and "time_seconds" in prediction else 0.0
                elif "time_seconds" in truth and "time_seconds" in prediction:
                    distance = abs(truth["time_seconds"] - prediction["time_seconds"])
                else:
                    continue
                if not exact_beat and distance > pairing_window:
                    continue
                candidates.append((0 if exact_beat else 1, distance, truth_index, prediction_index))
        candidates.sort()
        truth_used = [False] * len(ground_truth)
        prediction_used = [False] * len(predictions)
        for _exact_order, _distance, truth_index, prediction_index in candidates:
            if truth_used[truth_index] or prediction_used[prediction_index]:
                continue
            truth_used[truth_index] = True
            prediction_used[prediction_index] = True
            matches.append(_build_match(truth_index, prediction_index))
        missing = [index for index, used in enumerate(truth_used) if not used]
        extra = [index for index, used in enumerate(prediction_used) if not used]
    context = {"ground_truth": ground_truth, "predictions": predictions, "matches": matches, "missing": missing, "extra": extra}
    names = sorted(set([item["measurement"]["name"] for item in ground_truth] + [item["name"] for item in predictions]))
    channels = sorted(set([item["measurement"].get("channel", "") for item in ground_truth] + [item.get("channel", "") for item in predictions]))
    pairs = sorted(set([(item["measurement"]["name"], item["measurement"].get("channel", "")) for item in ground_truth] + [(item["name"], item.get("channel", "")) for item in predictions]))
    descriptors = sorted(set([_descriptor(item["measurement"]) for item in ground_truth] + [_descriptor(item) for item in predictions]))
    return {
        "schema_version": 2,
        "contract": "synsigra_measurement_score_v2",
        "score_type": "measurement_qa",
        "target": target,
        "options": {"pairing_window_seconds": pairing_window},
        "tolerance_rules": _tolerance_rules(ground_truth),
        "overall": _metrics(context),
        "by_measurement": [{"name": name, "metrics": _metrics(context, name=name)} for name in names],
        "by_channel": [{"channel": channel or "global", "metrics": _metrics(context, channel=channel)} for channel in channels],
        "by_measurement_channel": [{"name": name, "channel": channel or "global", "metrics": _metrics(context, name=name, channel=channel)} for name, channel in pairs],
        "by_measurement_context": [_context_result(descriptor, _metrics(context, descriptor=descriptor)) for descriptor in descriptors],
        "matches": matches,
        "missing_ground_truth_indices": missing,
        "extra_prediction_indices": extra,
        "messages": [],
    }


def measurement_comparison_csv(report):
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(["row_type", "name", "scope", "channel", "formula", "method_id", "preprocessing_policy_id", "window_start_seconds", "window_end_seconds", "ground_truth_count", "prediction_count", "matched_count", "truth_match_fraction", "prediction_match_fraction", "numeric_pair_count", "tolerance_pass_count", "tolerance_pass_fraction", "status_mismatch_count", "missing_count", "extra_count", "bias", "mean_absolute_error", "root_mean_square_error", "p95_absolute_error"])
    rows = [("overall", {}, report["overall"])] + [("measurement_context", item, item["metrics"]) for item in report["by_measurement_context"]]
    for row_type, item, metrics in rows:
        error = metrics["error"]
        writer.writerow([row_type, item.get("name", ""), item.get("scope", ""), item.get("channel", ""), item.get("formula", ""), item.get("method_id", ""), item.get("preprocessing_policy_id", ""), item.get("window_start_seconds", ""), item.get("window_end_seconds", ""), metrics["ground_truth_count"], metrics["prediction_count"], metrics["matched_count"], _optional(metrics["truth_match_fraction"]), _optional(metrics["prediction_match_fraction"]), metrics["numeric_pair_count"], metrics["tolerance_pass_count"], _optional(metrics["tolerance_pass_fraction"]), metrics["status_mismatch_count"], metrics["missing_count"], metrics["extra_count"], _optional(error["bias"]), _optional(error["mean_absolute"]), _optional(error["root_mean_square"]), _optional(error["p95_absolute"])])
    return output.getvalue()


def measurement_comparison_html(report):
    rows = []
    for item in report["by_measurement_context"]:
        metrics = item["metrics"]
        window = ""
        if "window_start_seconds" in item:
            window = "[%s, %s)" % (_optional(item["window_start_seconds"]), _optional(item["window_end_seconds"]))
        unit = item.get("unit", "")
        rows.append("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" % (
            html.escape(item["name"]), html.escape(unit or "unitless"), html.escape(item["scope"]),
            html.escape(item.get("method_id", "")), html.escape(item.get("preprocessing_policy_id", "")),
            html.escape(window), metrics["ground_truth_count"], metrics["prediction_count"],
            metrics["numeric_pair_count"], _optional(metrics["tolerance_pass_fraction"]),
            html.escape(_context_tolerance_rule(item, report.get("tolerance_rules", []))),
            metrics["missing_count"], metrics["extra_count"],
            html.escape(_measurement_with_unit(metrics["error"]["mean_absolute"], unit)),
        ))
    return "<!doctype html><html><head><meta charset=\"utf-8\"><title>Measurement QA</title><style>body{font:14px Arial,sans-serif;color:#202124;max-width:1400px;margin:32px auto;padding:0 20px}table{border-collapse:collapse;width:100%%}th,td{border:1px solid #d1d5db;padding:7px;text-align:left}th{background:#f3f4f6}.notice{border-left:4px solid #6b7280;padding:10px 14px;background:#f3f4f6;color:#374151}.help{color:#5f6b7a}</style></head><body><h1>Measurement QA Report</h1><p class=\"notice\">Synthetic engineering QA evidence; not diagnosis, nor clinical evidence</p><p>Target: %s | Pairing window: %.6g s</p><p class=\"help\">The pairing window identifies corresponding rows. Each numeric pair passes when |submitted − reference| is within the larger packaged absolute-or-relative tolerance shown below.</p><table><tr><th>Measurement</th><th>Unit</th><th>Scope</th><th>Method</th><th>Preprocessing</th><th>Window</th><th>Reference</th><th>Submitted</th><th>Numeric pairs</th><th>Within tolerance</th><th>Tolerance rule</th><th>Missing</th><th>Extra</th><th>MAE</th></tr>%s</table></body></html>" % (html.escape(report["target"]), report["options"]["pairing_window_seconds"], "".join(rows))


def _context_tolerance_rule(context, rules):
    fields = ("name", "unit", "scope", "channel", "formula", "method_id", "preprocessing_policy_id")
    matches = [item for item in rules if all((item.get(name, "") or "") == (context.get(name, "") or "") for name in fields)]
    values = []
    for item in matches:
        absolute = "±%s absolute" % _measurement_with_unit(item["absolute_tolerance"], item.get("unit", ""))
        relative = "±%.6g%% of |reference|" % item["relative_tolerance_percent"]
        value = "larger of %s or %s" % (absolute, relative) if item["absolute_tolerance"] > 0.0 and item["relative_tolerance_percent"] > 0.0 else relative if item["relative_tolerance_percent"] > 0.0 else absolute
        if value not in values:
            values.append(value)
    return "; ".join(values) if values else "not applicable"


def _measurement_with_unit(value, unit):
    if value is None:
        return "—"
    return "%.6g%s" % (float(value), (" " + unit) if unit else "")


def _metrics(context, name=None, channel=None, descriptor=None):
    truth_selected = [index for index, item in enumerate(context["ground_truth"]) if (name is None or item["measurement"]["name"] == name) and (channel is None or item["measurement"].get("channel", "") == channel) and (descriptor is None or _descriptor(item["measurement"]) == descriptor)]
    prediction_selected = [index for index, item in enumerate(context["predictions"]) if (name is None or item["name"] == name) and (channel is None or item.get("channel", "") == channel) and (descriptor is None or _descriptor(item) == descriptor)]
    truth_set, prediction_set = set(truth_selected), set(prediction_selected)
    selected_matches = [item for item in context["matches"] if item["ground_truth_index"] in truth_set and item["prediction_index"] in prediction_set]
    statuses = [context["ground_truth"][index]["measurement"]["status"] for index in truth_selected]
    errors = [item["signed_error"] for item in selected_matches if item["numeric_pair"]]
    absolute = sorted(abs(item) for item in errors)
    assertions = [item for item in selected_matches if "ground_truth_assertion_passed" in item]
    numeric = len(errors)
    matched = len(selected_matches)
    covered_truth_count = len(set(item["ground_truth_index"] for item in selected_matches))
    matched_prediction_count = len(set(item["prediction_index"] for item in selected_matches))
    metrics = {
        "ground_truth_count": len(truth_selected), "valid_truth_count": statuses.count("valid"), "undefined_truth_count": statuses.count("undefined"), "absent_truth_count": statuses.count("absent"), "not_evaluable_truth_count": statuses.count("not_evaluable"),
        "prediction_count": len(prediction_selected), "matched_count": matched, "numeric_pair_count": numeric,
        "covered_truth_count": covered_truth_count, "matched_prediction_count": matched_prediction_count,
        "tolerance_pass_count": sum(1 for item in selected_matches if item.get("within_tolerance", False)), "status_match_count": sum(1 for item in selected_matches if item["status_matches"]), "status_mismatch_count": sum(1 for item in selected_matches if not item["status_matches"]),
        "missing_count": sum(1 for index in context["missing"] if index in truth_set), "extra_count": sum(1 for index in context["extra"] if index in prediction_set),
        "assertion_comparable_count": len(assertions), "assertion_agreement_count": sum(1 for item in assertions if item["ground_truth_assertion_passed"] == item["prediction_assertion_passed"]),
        "tolerance_pass_fraction": None, "status_match_fraction": None, "assertion_agreement_fraction": None, "truth_match_fraction": None, "prediction_match_fraction": None,
        "error": {"bias": None, "mean_absolute": None, "root_mean_square": None, "median_absolute": None, "p95_absolute": None, "maximum_absolute": None},
    }
    metrics["tolerance_pass_fraction"] = float(metrics["tolerance_pass_count"]) / numeric if numeric else None
    metrics["status_match_fraction"] = float(metrics["status_match_count"]) / matched if matched else None
    metrics["assertion_agreement_fraction"] = float(metrics["assertion_agreement_count"]) / len(assertions) if assertions else None
    # Coverage fractions count UNIQUE associated records, so a many-to-many peak-anchored
    # association (one truth compared to several submitted fragments, or vice versa) still
    # reports coverage in [0, 1] rather than exceeding it.
    metrics["truth_match_fraction"] = float(covered_truth_count) / len(truth_selected) if truth_selected else None
    metrics["prediction_match_fraction"] = float(matched_prediction_count) / len(prediction_selected) if prediction_selected else None
    if errors:
        metrics["error"] = {"bias": sum(errors) / len(errors), "mean_absolute": sum(absolute) / len(absolute), "root_mean_square": math.sqrt(sum(item * item for item in absolute) / len(absolute)), "median_absolute": _median(absolute), "p95_absolute": absolute[int(math.ceil(0.95 * len(absolute))) - 1], "maximum_absolute": absolute[-1]}
    return metrics


def _validate_measurement(item, index):
    if not isinstance(item, dict) or not set(item).issubset(set(MEASUREMENT_FIELDS)):
        raise MeasurementError("measurement %s contains unknown fields or is not an object" % index)
    for field in ("name", "unit", "status", "scope"):
        if not isinstance(item.get(field), str):
            raise MeasurementError("measurement %s requires string field %s" % (index, field))
    output = dict(item)
    if not re.match(r"^[a-z][a-z0-9_.]{0,127}$", output["name"]):
        raise MeasurementError("measurement name must be lower-case ASCII dotted snake case")
    if output["unit"] not in MEASUREMENT_UNITS or output["status"] not in MEASUREMENT_STATUSES or output["scope"] not in MEASUREMENT_SCOPES:
        raise MeasurementError("measurement %s contains unsupported unit, status, or scope" % index)
    for field in ("value", "time_seconds", "window_start_seconds", "window_end_seconds", "confidence"):
        if field in output:
            output[field] = _finite_number(output[field], field)
    if output["status"] == "valid" and "value" not in output:
        raise MeasurementError("valid measurement requires value")
    if output["status"] != "valid" and "value" in output:
        raise MeasurementError("non-valid measurement must not contain value")
    if output["unit"] == "bool" and "value" in output and output["value"] not in (0.0, 1.0):
        raise MeasurementError("bool measurement value must be zero or one")
    if output.get("time_seconds", 0.0) < 0.0 or not 0.0 <= output.get("confidence", 0.0) <= 1.0:
        raise MeasurementError("measurement time or confidence is out of range")
    if "beat_index" in output and (not isinstance(output["beat_index"], str) or not re.match(r"^(0|[1-9][0-9]*)$", output["beat_index"]) or int(output["beat_index"]) > 18446744073709551615):
        raise MeasurementError("beat_index must be a canonical unsigned decimal string")
    for field, limit in (("channel", 128), ("formula", 64)):
        if field in output and (not isinstance(output[field], str) or len(output[field]) > limit or any(ord(ch) < 32 for ch in output[field])):
            raise MeasurementError("measurement %s has invalid %s" % (index, field))
    for field in ("method_id", "preprocessing_policy_id"):
        if field in output and (not isinstance(output[field], str) or not re.match(r"^[a-z][a-z0-9_.]{0,127}$", output[field])):
            raise MeasurementError("measurement %s has invalid %s" % (index, field))
    beat_like = output["scope"] in ("beat", "beat_lead", "paired_signal")
    window_like = output["scope"] in ("window", "window_lead")
    channel_scope = output["scope"] in ("lead", "beat_lead", "paired_signal", "window_lead")
    if beat_like != ("time_seconds" in output or "beat_index" in output):
        raise MeasurementError("measurement scope has an invalid temporal anchor")
    if channel_scope != bool(output.get("channel", "")):
        raise MeasurementError("measurement scope has an invalid channel")
    if window_like != ("window_start_seconds" in output and "window_end_seconds" in output):
        raise MeasurementError("measurement scope has an invalid window")
    if ("window_start_seconds" in output) != ("window_end_seconds" in output):
        raise MeasurementError("measurement window endpoints must occur together")
    if window_like and (output["window_start_seconds"] < 0.0 or output["window_end_seconds"] <= output["window_start_seconds"]):
        raise MeasurementError("measurement window must satisfy 0 <= start < end")
    if output["name"] == "qtc_interval" and output.get("formula") not in QT_FORMULAS:
        raise MeasurementError("qtc_interval requires a supported formula")
    return output


def _csv_record(row, index):
    output = dict((name, value) for name, value in zip(MEASUREMENT_FIELDS, row) if value != "")
    for field in ("value", "time_seconds", "window_start_seconds", "window_end_seconds", "confidence"):
        if field in output:
            output[field] = _finite_number(output[field], field)
    return output


def _require_unique(records):
    identities = set()
    for item in records:
        identity = _identity(item)
        if identity in identities:
            raise MeasurementError("duplicate measurement identity")
        identities.add(identity)


def _identity(item):
    anchor = "b" + item["beat_index"] if "beat_index" in item else "t" + repr(item["time_seconds"]) if "time_seconds" in item else "w" + repr(item["window_start_seconds"]) + ":" + repr(item["window_end_seconds"]) if "window_start_seconds" in item else "r"
    return _descriptor(item) + (anchor,)


def _descriptor(item):
    return item["name"], item["unit"], item["scope"], item.get("channel", ""), item.get("formula", ""), item.get("method_id", ""), item.get("preprocessing_policy_id", ""), item.get("window_start_seconds"), item.get("window_end_seconds")


def _context_result(descriptor, metrics):
    name, unit, scope, channel, formula, method_id, preprocessing_policy_id, window_start, window_end = descriptor
    output = {"name": name, "unit": unit, "scope": scope, "channel": channel or "global", "formula": formula, "method_id": method_id, "preprocessing_policy_id": preprocessing_policy_id, "metrics": metrics}
    if window_start is not None:
        output["window_start_seconds"] = window_start
        output["window_end_seconds"] = window_end
    return output


def _tolerance_rules(ground_truth):
    grouped = {}
    for item in ground_truth:
        measurement = item["measurement"]
        key = (
            measurement["name"],
            measurement["unit"],
            measurement["scope"],
            measurement.get("channel", "") or "global",
            measurement.get("formula", ""),
            measurement.get("method_id", ""),
            measurement.get("preprocessing_policy_id", ""),
            float(item["absolute_tolerance"]),
            float(item["relative_tolerance_percent"]),
            item["error_model"],
        )
        grouped[key] = grouped.get(key, 0) + 1
    output = []
    for key in sorted(grouped):
        (
            name, unit, scope, channel, formula, method_id,
            preprocessing_policy_id, absolute_tolerance,
            relative_tolerance_percent, error_model,
        ) = key
        output.append({
            "name": name,
            "unit": unit,
            "scope": scope,
            "channel": channel,
            "formula": formula,
            "method_id": method_id,
            "preprocessing_policy_id": preprocessing_policy_id,
            "absolute_tolerance": absolute_tolerance,
            "relative_tolerance_percent": relative_tolerance_percent,
            "error_model": error_model,
            "reference_value_count": grouped[key],
        })
    return output


def _finite_number(value, name):
    if isinstance(value, bool):
        raise MeasurementError("%s must be a finite number" % name)
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        raise MeasurementError("%s must be a finite number" % name)
    if not math.isfinite(numeric):
        raise MeasurementError("%s must be a finite number" % name)
    return numeric


def _unique_object(pairs):
    output = {}
    for key, value in pairs:
        if key in output:
            raise MeasurementError("duplicate JSON object key: %s" % key)
        output[key] = value
    return output


def _median(values):
    middle = len(values) // 2
    return values[middle] if len(values) & 1 else 0.5 * (values[middle - 1] + values[middle])


def _optional(value):
    return "NA" if value is None else value
