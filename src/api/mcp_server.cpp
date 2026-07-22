#include "syn_sig_ra/mcp_server.h"

#include "syn_sig_ra/api_key_auth.h"
#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/pack_catalog.h"

#include <jansson.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace {

const char kLatestProtocol[] = "2025-11-25";

const char kToolsJson[] = R"JSON([
  {
    "name":"synsigra_recommend_packs",
    "title":"Recommend Synsigra challenge packs",
    "description":"Translate an algorithm-QA goal into current curated-pack candidates. Always call this before choosing a curated pack. It reports scoreable, reference-only and missing target coverage and tells you when exact duration, sampling rate or target requirements require the custom-authoring tools instead.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["goal"],"properties":{"goal":{"type":"string","minLength":1,"description":"Plain-language engineering goal. Do not include patient data or PHI."},"desired_targets":{"type":"array","uniqueItems":true,"items":{"type":"string","enum":["r_peak","rr_interval","ecg_beat_classification","rhythm_episode","rhythm_burden","ecg_delineation","qtc","morphology_assertions","hrv","signal_quality","ppg_systolic_peak","ppg_pulse_onset","ecg_ppg_alignment","ppg_optical","prv","respiratory_rate"]}},"duration_seconds":{"type":"integer","minimum":1},"sampling_rate_hz":{"type":"integer","minimum":1},"prefer_scoreable":{"type":"boolean","default":true},"max_results":{"type":"integer","minimum":1,"maximum":10,"default":5}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_list_packs","title":"List current curated packs",
    "description":"List concise metadata for current core-backed curated challenge packs. Use recommend_packs for goal-based ranking and get_pack before generation.",
    "inputSchema":{"type":"object","additionalProperties":false,"properties":{"query":{"type":"string"},"target":{"type":"string"},"scoreable_only":{"type":"boolean","default":false}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_get_pack","title":"Inspect one curated pack",
    "description":"Return authoritative pack details, scenarios, scoreable and reference-only targets, sampling rates, duration, use recommendations, contracts and verification metadata.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["pack_id"],"properties":{"pack_id":{"type":"string","minLength":1}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_list_projects","title":"List available projects",
    "description":"List projects visible to this API key. Select a project_id from this result before creating a job; never guess or hard-code it.",
    "inputSchema":{"type":"object","additionalProperties":false,"properties":{}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_create_job","title":"Generate a challenge package",
    "description":"Queue generation from an existing curated or custom pack. This consumes quota and creates a job. Call only after the user has approved the selected pack and project, then poll get_job until succeeded or failed.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["project_id","pack_id"],"properties":{"project_id":{"type":"string","minLength":1},"pack_id":{"type":"string","minLength":1}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
  },
  {
    "name":"synsigra_get_job","title":"Check generation status",
    "description":"Read a job's status, reproducibility identity, challenge contract and artifact links. Poll with restraint; stop on succeeded, failed or cancelled.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["job_id"],"properties":{"job_id":{"type":"string","pattern":"^job_[A-Za-z0-9_-]+$"}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_list_jobs","title":"List recent generation jobs",
    "description":"List organization jobs with bounded pagination.",
    "inputSchema":{"type":"object","additionalProperties":false,"properties":{"limit":{"type":"integer","minimum":1,"maximum":100,"default":25},"offset":{"type":"integer","minimum":0,"default":0}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_get_verification_guide","title":"Get the exact local verification runbook",
    "description":"Return a concise, mode-aware local runbook for one job: authenticated kit and verifier downloads, the exact synsigra-verify command, report entry points, exit codes and the evidence archive checklist. Proprietary algorithm output always remains local.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["job_id"],"properties":{"job_id":{"type":"string","pattern":"^job_[A-Za-z0-9_-]+$"}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_rebuild_expired_job","title":"Rebuild an expired package exactly",
    "description":"Queue a fingerprint-verified rebuild using the preserved recipe and exact historical generator after a succeeded job's cached artifact expires. This creates a new job and consumes quota; it never substitutes the latest generator.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["job_id"],"properties":{"job_id":{"type":"string","pattern":"^job_[A-Za-z0-9_-]+$"}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
  },
  {
    "name":"synsigra_get_authoring_contract","title":"Get live custom-authoring schema and templates",
    "description":"Fetch the core-owned authoring schema or template catalog. Call before custom authoring; never invent fields, enum values, artifact types, conditions or ranges.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["document"],"properties":{"document":{"type":"string","enum":["schema","templates"]}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_get_curated_scenario","title":"Clone a curated scenario",
    "description":"Fetch one complete curated scenario as a safe starting point for custom duration, sampling, physiology or artifacts. Obtain pack_id and case_id from get_pack.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["pack_id","case_id"],"properties":{"pack_id":{"type":"string","minLength":1},"case_id":{"type":"string","minLength":1}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_preview_scenario","title":"Validate and analyze a scenario",
    "description":"Authoritatively validate a complete scenario against its exact final target list before saving it. Fix TARGET_INCOMPATIBLE errors. REFERENCE_ONLY_TARGET is a claim-boundary warning, not a scoring success.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["scenario","targets"],"properties":{"scenario":{"type":"object"},"targets":{"type":"array","minItems":1,"uniqueItems":true,"items":{"type":"string"}}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
  },
  {
    "name":"synsigra_save_scenario","title":"Save a validated scenario draft",
    "description":"Save a user-owned scenario draft after preview. Invalid drafts are retained with validation errors; use the returned scenario_id only when status is valid.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["name","scenario","target_intent"],"properties":{"name":{"type":"string","minLength":1,"maxLength":100},"scenario":{"type":"object"},"target_intent":{"type":"array","minItems":1,"uniqueItems":true,"items":{"type":"string"}}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
  },
  {
    "name":"synsigra_create_custom_pack","title":"Compose a custom pack",
    "description":"Snapshot valid owned scenario drafts into one immutable custom pack. Every selected scenario must satisfy every target. This creates metadata but does not generate signals until create_job is called.",
    "inputSchema":{"type":"object","additionalProperties":false,"required":["name","description","targets","scenario_ids"],"properties":{"name":{"type":"string","minLength":1,"maxLength":100},"description":{"type":"string"},"targets":{"type":"array","minItems":1,"uniqueItems":true,"items":{"type":"string"}},"scenario_ids":{"type":"array","minItems":1,"uniqueItems":true,"items":{"type":"string","pattern":"^scenario_[A-Za-z0-9_-]+$"}}}},
    "outputSchema":{"type":"object","additionalProperties":true},
    "annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
  }
])JSON";

std::string lower(std::string value) {
    for (std::string::iterator it = value.begin(); it != value.end(); ++it) {
        *it = static_cast<char>(std::tolower(static_cast<unsigned char>(*it)));
    }
    return value;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string dump_json(json_t* value) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_SORT_KEYS);
    const std::string result = encoded == nullptr ? "{}" : encoded;
    free(encoded);
    return result;
}

syn_sig_ra::RouteResponse http_json(int status, json_t* value) {
    syn_sig_ra::RouteResponse response;
    response.disposition = syn_sig_ra::RouteDisposition::handled;
    response.status = status;
    response.content_type = "application/json; charset=utf-8";
    response.cache_control = "no-store";
    response.body = value == nullptr ? std::string() : dump_json(value) + "\n";
    return response;
}

json_t* rpc_envelope(json_t* id) {
    json_t* root = json_object();
    json_object_set_new(root, "jsonrpc", json_string("2.0"));
    if (id == nullptr) {
        json_object_set_new(root, "id", json_null());
    } else {
        json_object_set(root, "id", id);
    }
    return root;
}

syn_sig_ra::RouteResponse rpc_error(
    int http_status,
    json_t* id,
    int code,
    const std::string& message
) {
    json_t* root = rpc_envelope(id);
    json_t* error = json_object();
    json_object_set_new(error, "code", json_integer(code));
    json_object_set_new(error, "message", json_string(message.c_str()));
    json_object_set_new(root, "error", error);
    syn_sig_ra::RouteResponse response = http_json(http_status, root);
    json_decref(root);
    return response;
}

syn_sig_ra::RouteResponse rpc_result(json_t* id, json_t* result) {
    json_t* root = rpc_envelope(id);
    json_object_set_new(root, "result", result);
    syn_sig_ra::RouteResponse response = http_json(200, root);
    json_decref(root);
    return response;
}

std::string origin_of(const std::string& url) {
    const std::string::size_type scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    const std::string::size_type path = url.find('/', scheme + 3);
    return path == std::string::npos ? url : url.substr(0, path);
}

std::string absolute_url(
    const syn_sig_ra::EmailConfig& config,
    const std::string& relative
) {
    if (relative.compare(0, 7, "http://") == 0 ||
        relative.compare(0, 8, "https://") == 0) {
        return relative;
    }
    const std::string origin = origin_of(config.public_origin);
    return origin.empty() ? relative : origin + relative;
}

bool accepts_mcp(const std::string& value) {
    const std::string normalized = lower(value);
    return contains(normalized, "application/json") &&
           contains(normalized, "text/event-stream");
}

bool json_content_type(const std::string& value) {
    const std::string normalized = lower(value);
    return normalized == "application/json" ||
           normalized.compare(0, 17, "application/json;") == 0;
}

bool supported_protocol(const std::string& value) {
    return value == "2025-11-25" || value == "2025-06-18" ||
           value == "2025-03-26";
}

std::string string_field(json_t* object, const char* name) {
    json_t* value = json_is_object(object) ? json_object_get(object, name) : nullptr;
    return json_is_string(value) ? json_string_value(value) : std::string();
}

int integer_field(json_t* object, const char* name, int fallback) {
    json_t* value = json_is_object(object) ? json_object_get(object, name) : nullptr;
    return json_is_integer(value) ? static_cast<int>(json_integer_value(value))
                                  : fallback;
}

bool bool_field(json_t* object, const char* name, bool fallback) {
    json_t* value = json_is_object(object) ? json_object_get(object, name) : nullptr;
    return json_is_boolean(value) ? json_is_true(value) : fallback;
}

json_t* strings_json(const std::vector<std::string>& values) {
    json_t* array = json_array();
    for (std::vector<std::string>::const_iterator it = values.begin();
         it != values.end(); ++it) {
        json_array_append_new(array, json_string(it->c_str()));
    }
    return array;
}

bool has_target(
    const std::vector<syn_sig_ra::PackTargetSummary>& values,
    const std::string& target
) {
    for (std::vector<syn_sig_ra::PackTargetSummary>::const_iterator it =
             values.begin(); it != values.end(); ++it) {
        if (it->target == target) return true;
    }
    return false;
}

bool has_string(
    const std::vector<std::string>& values,
    const std::string& target
) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

void infer_target(
    const std::string& goal,
    const std::vector<std::string>& needles,
    const std::string& target,
    std::set<std::string>& targets
) {
    for (std::vector<std::string>::const_iterator it = needles.begin();
         it != needles.end(); ++it) {
        if (contains(goal, *it)) {
            targets.insert(target);
            return;
        }
    }
}

std::vector<std::string> requested_targets(json_t* arguments) {
    std::set<std::string> targets;
    json_t* explicit_targets = json_is_object(arguments)
        ? json_object_get(arguments, "desired_targets") : nullptr;
    if (json_is_array(explicit_targets)) {
        std::size_t index = 0;
        json_t* value = nullptr;
        json_array_foreach(explicit_targets, index, value) {
            if (json_is_string(value)) targets.insert(json_string_value(value));
        }
    }
    if (!targets.empty()) {
        return std::vector<std::string>(targets.begin(), targets.end());
    }
    const std::string goal = lower(string_field(arguments, "goal"));
    infer_target(goal, {"r peak", "r-peak", "peak detection", "csucsdetekt", "csúcsdetekt"}, "r_peak", targets);
    infer_target(goal, {"rr interval", "rr-inter", "rr,", " rr ", "rr ért"}, "rr_interval", targets);
    infer_target(goal, {"hrv", "sdnn", "rmssd", "mrssd", "lf/hf", "lf ", "hf "}, "hrv", targets);
    infer_target(goal, {"beat class", "pvc", "pac", "ütésoszt", "utesoszt"}, "ecg_beat_classification", targets);
    infer_target(goal, {"arrhythm", "aritmi", "rhythm episode", "ritmus"}, "rhythm_episode", targets);
    infer_target(goal, {"burden", "terhelés", "terheles"}, "rhythm_burden", targets);
    infer_target(goal, {"delineat", "wave boundary", "hullámhat", "hullamhat"}, "ecg_delineation", targets);
    infer_target(goal, {"qtc", "qt interval"}, "qtc", targets);
    infer_target(goal, {"morpholog", "morfol"}, "morphology_assertions", targets);
    infer_target(goal, {"noise", "artifact", "signal quality", "zaj", "jelmin"}, "signal_quality", targets);
    infer_target(goal, {"ppg peak", "systolic peak", "szisztolés", "szisztoles"}, "ppg_systolic_peak", targets);
    infer_target(goal, {"pulse onset", "pulzuskezdet"}, "ppg_pulse_onset", targets);
    infer_target(goal, {"ecg ppg", "alignment", "igazítás", "igazitas"}, "ecg_ppg_alignment", targets);
    infer_target(goal, {"optical", "optikai"}, "ppg_optical", targets);
    infer_target(goal, {"prv"}, "prv", targets);
    infer_target(goal, {"respiratory rate", "légzésszám", "legzesszam"}, "respiratory_rate", targets);
    return std::vector<std::string>(targets.begin(), targets.end());
}

json_t* concise_pack(const syn_sig_ra::PackSummary& pack) {
    json_t* value = json_object();
    json_object_set_new(value, "pack_id", json_string(pack.pack_id.c_str()));
    json_object_set_new(value, "name", json_string(pack.display_name.c_str()));
    json_object_set_new(value, "description", json_string(pack.description.c_str()));
    json_object_set_new(value, "targets", strings_json(pack.targets));
    std::vector<std::string> scoreable;
    for (std::vector<syn_sig_ra::PackTargetSummary>::const_iterator it =
             pack.scoreable_targets.begin(); it != pack.scoreable_targets.end(); ++it) {
        scoreable.push_back(it->target);
    }
    std::vector<std::string> reference;
    for (std::vector<syn_sig_ra::PackTargetSummary>::const_iterator it =
             pack.reference_only_targets.begin();
         it != pack.reference_only_targets.end(); ++it) {
        reference.push_back(it->target);
    }
    json_object_set_new(value, "scoreable_targets", strings_json(scoreable));
    json_object_set_new(value, "reference_only_targets", strings_json(reference));
    json_object_set_new(value, "sampling_rates_hz", [&pack]() {
        json_t* rates = json_array();
        for (std::vector<int>::const_iterator it = pack.sampling_rates_hz.begin();
             it != pack.sampling_rates_hz.end(); ++it) {
            json_array_append_new(rates, json_integer(*it));
        }
        return rates;
    }());
    json_object_set_new(value, "total_seconds", json_integer(pack.total_seconds));
    json_object_set_new(value, "case_seconds_min", json_integer(pack.minimum_case_seconds));
    json_object_set_new(value, "case_seconds_max", json_integer(pack.maximum_case_seconds));
    json_object_set_new(value, "recommended_for", strings_json(pack.recommended_for));
    json_object_set_new(value, "difficulty", strings_json(pack.difficulty));
    return value;
}

struct RankedPack {
    const syn_sig_ra::PackSummary* pack;
    int score;
    bool requirements_satisfied;
    std::vector<std::string> scoreable;
    std::vector<std::string> reference;
    std::vector<std::string> missing;
    std::vector<std::string> notes;
};

json_t* recommend_packs(
    const std::string& pack_root,
    json_t* arguments,
    std::string& error
) {
    std::vector<syn_sig_ra::PackSummary> packs;
    if (!syn_sig_ra::PackCatalog(pack_root).list(packs, error)) return nullptr;
    const std::vector<std::string> targets = requested_targets(arguments);
    const int requested_duration = integer_field(arguments, "duration_seconds", 0);
    const int requested_rate = integer_field(arguments, "sampling_rate_hz", 0);
    const bool prefer_scoreable = bool_field(arguments, "prefer_scoreable", true);
    int max_results = integer_field(arguments, "max_results", 5);
    if (max_results < 1) max_results = 1;
    if (max_results > 10) max_results = 10;
    const std::string goal = lower(string_field(arguments, "goal"));
    std::vector<RankedPack> ranked;
    for (std::vector<syn_sig_ra::PackSummary>::const_iterator pack = packs.begin();
         pack != packs.end(); ++pack) {
        RankedPack item;
        item.pack = &*pack;
        item.score = 0;
        item.requirements_satisfied = true;
        for (std::vector<std::string>::const_iterator target = targets.begin();
             target != targets.end(); ++target) {
            if (has_target(pack->scoreable_targets, *target)) {
                item.score += 120;
                item.scoreable.push_back(*target);
            } else if (has_target(pack->reference_only_targets, *target)) {
                item.score += prefer_scoreable ? 10 : 45;
                item.reference.push_back(*target);
                if (prefer_scoreable) item.requirements_satisfied = false;
            } else if (has_string(pack->targets, *target)) {
                item.score += 30;
                item.reference.push_back(*target);
                if (prefer_scoreable) item.requirements_satisfied = false;
            } else {
                item.score -= 160;
                item.missing.push_back(*target);
                item.requirements_satisfied = false;
            }
        }
        std::string searchable = lower(
            pack->pack_id + " " + pack->display_name + " " + pack->description);
        for (std::vector<std::string>::const_iterator it = pack->feature_tags.begin();
             it != pack->feature_tags.end(); ++it) searchable += " " + lower(*it);
        std::istringstream words(goal);
        std::string word;
        int lexical = 0;
        while (words >> word) {
            if (word.size() >= 4 && contains(searchable, word)) lexical += 2;
        }
        item.score += std::min(lexical, 30);
        if (requested_rate > 0) {
            if (std::find(pack->sampling_rates_hz.begin(),
                          pack->sampling_rates_hz.end(), requested_rate) !=
                pack->sampling_rates_hz.end()) {
                item.score += 30;
            } else {
                item.score -= 35;
                item.requirements_satisfied = false;
                item.notes.push_back("requested sampling rate is not present");
            }
        }
        if (requested_duration > 0) {
            const int difference = std::abs(pack->total_seconds - requested_duration);
            const int tolerance = std::max(1, requested_duration / 20);
            if (difference <= tolerance) {
                item.score += 30;
            } else {
                item.score -= std::min(40, difference / 10 + 5);
                item.requirements_satisfied = false;
                item.notes.push_back("requested total duration is not matched");
            }
        }
        ranked.push_back(item);
    }
    std::sort(ranked.begin(), ranked.end(), [](const RankedPack& left, const RankedPack& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.pack->pack_id < right.pack->pack_id;
    });
    json_t* root = json_object();
    json_object_set_new(root, "interpreted_targets", strings_json(targets));
    json_object_set_new(root, "target_inference",
        json_string(json_is_array(json_object_get(arguments, "desired_targets"))
            ? "explicit" : "inferred_from_goal"));
    json_t* candidates = json_array();
    const std::size_t count = std::min(
        ranked.size(), static_cast<std::size_t>(max_results));
    bool best_satisfied = false;
    for (std::size_t index = 0; index < count; ++index) {
        json_t* value = concise_pack(*ranked[index].pack);
        json_object_set_new(value, "rank", json_integer(index + 1));
        json_object_set_new(value, "match_score", json_integer(ranked[index].score));
        json_object_set_new(value, "requirements_satisfied",
                            json_boolean(ranked[index].requirements_satisfied));
        json_object_set_new(value, "requested_scoreable_targets",
                            strings_json(ranked[index].scoreable));
        json_object_set_new(value, "requested_reference_only_targets",
                            strings_json(ranked[index].reference));
        json_object_set_new(value, "missing_requested_targets",
                            strings_json(ranked[index].missing));
        json_object_set_new(value, "constraint_notes", strings_json(ranked[index].notes));
        json_array_append_new(candidates, value);
        if (index == 0) best_satisfied = ranked[index].requirements_satisfied;
    }
    json_object_set_new(root, "candidates", candidates);
    json_object_set_new(root, "recommended_workflow", json_string(
        best_satisfied ? "inspect_top_curated_pack_then_create_job"
                       : "use_custom_authoring_for_unmet_requirements"));
    json_object_set_new(root, "next_step", json_string(
        best_satisfied
            ? "Call synsigra_get_pack for the top candidate and ask the user to approve it."
            : "Fetch the live authoring schema/templates, clone the closest scenario, change only requested fields, preview it, then save and compose a custom pack."));
    json_object_set_new(root, "claim_boundary", json_string(
        "Reference-only targets provide ground truth but no automated local score. Missing targets are not validated by that pack."));
    return root;
}

syn_sig_ra::RouteResponse invoke_api(
    const std::string& method,
    const std::string& uri,
    const std::string& query,
    const std::string& body,
    const std::string& public_base_path,
    const std::string& authorization_header,
    syn_sig_ra::MetadataStore* metadata_store,
    const std::string& pack_root,
    const std::string& data_root,
    const std::string& signal_synth_cli,
    const syn_sig_ra::EmailConfig& email_config
) {
    return syn_sig_ra::route_request(
        method, uri, public_base_path, authorization_header, metadata_store,
        pack_root, body.empty() ? std::string() : "application/json",
        body, data_root, query, signal_synth_cli, std::string(), email_config);
}

json_t* parsed_api_body(const syn_sig_ra::RouteResponse& response) {
    json_error_t error;
    json_t* value = json_loadb(
        response.body.data(), response.body.size(), JSON_REJECT_DUPLICATES, &error);
    if (value != nullptr) return value;
    value = json_object();
    json_object_set_new(value, "http_status", json_integer(response.status));
    json_object_set_new(value, "body", json_string(response.body.c_str()));
    return value;
}

syn_sig_ra::RouteResponse tool_response(
    json_t* id,
    json_t* structured,
    bool is_error,
    const std::string& text_override = std::string()
) {
    json_t* result = json_object();
    json_t* content = json_array();
    json_t* text = json_object();
    json_object_set_new(text, "type", json_string("text"));
    const std::string rendered = text_override.empty()
        ? dump_json(structured) : text_override;
    json_object_set_new(text, "text", json_string(rendered.c_str()));
    json_array_append_new(content, text);
    json_object_set_new(result, "content", content);
    json_object_set(result, "structuredContent", structured);
    if (is_error) json_object_set_new(result, "isError", json_true());
    return rpc_result(id, result);
}

syn_sig_ra::RouteResponse api_tool_response(
    json_t* id,
    const syn_sig_ra::RouteResponse& api
) {
    json_t* structured = parsed_api_body(api);
    json_object_set_new(structured, "http_status", json_integer(api.status));
    syn_sig_ra::RouteResponse result = tool_response(
        id, structured, api.status < 200 || api.status >= 300);
    json_decref(structured);
    return result;
}

json_t* copy_argument_object(json_t* arguments) {
    return json_is_object(arguments) ? json_deep_copy(arguments) : json_object();
}

std::string request_body_from(json_t* value) {
    const std::string body = dump_json(value);
    json_decref(value);
    return body;
}

json_t* concise_job_summary(json_t* job) {
    json_t* summary = json_object();
    const char* fields[] = {
        "job_id", "status", "project_id", "pack_id", "package_id",
        "artifact_status", "package_fingerprint", "generator_git_commit"
    };
    for (std::size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        const std::string value = string_field(job, fields[index]);
        if (!value.empty()) {
            json_object_set_new(summary, fields[index], json_string(value.c_str()));
        }
    }
    return summary;
}

json_t* concise_challenge_summary(json_t* challenge) {
    json_t* summary = json_object();
    const char* fields[] = {
        "challenge_contract", "local_verification_contract", "verifier_version"
    };
    for (std::size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        const std::string value = string_field(challenge, fields[index]);
        if (!value.empty()) {
            json_object_set_new(summary, fields[index], json_string(value.c_str()));
        }
    }
    json_t* case_count = json_is_object(challenge)
        ? json_object_get(challenge, "case_count") : nullptr;
    if (json_is_integer(case_count)) {
        json_object_set(summary, "case_count", case_count);
    }
    std::vector<std::string> targets;
    json_t* source_targets = json_is_object(challenge)
        ? json_object_get(challenge, "targets") : nullptr;
    if (json_is_array(source_targets)) {
        std::size_t index = 0;
        json_t* target = nullptr;
        json_array_foreach(source_targets, index, target) {
            const std::string name = string_field(target, "target");
            if (!name.empty() && !has_string(targets, name)) targets.push_back(name);
        }
    }
    json_object_set_new(summary, "targets", strings_json(targets));
    return summary;
}

}  // namespace

namespace syn_sig_ra {

RouteResponse handle_mcp_request(
    const std::string& method,
    const std::string& public_base_path,
    const std::string& authorization_header,
    MetadataStore* metadata_store,
    const std::string& pack_root,
    const std::string& data_root,
    const std::string& signal_synth_cli,
    const EmailConfig& email_config,
    const std::string& content_type,
    const std::string& request_body,
    const std::string& accept_header,
    const std::string& origin_header,
    const std::string& protocol_version_header
) {
    const std::string expected_origin = origin_of(email_config.public_origin);
    if (!origin_header.empty() &&
        (expected_origin.empty() || origin_header != expected_origin)) {
        return rpc_error(403, nullptr, -32000, "Forbidden Origin header.");
    }
    if (metadata_store == nullptr) {
        return rpc_error(503, nullptr, -32000, "Authentication storage is unavailable.");
    }
    const AuthenticationResult authentication =
        authenticate_bearer(authorization_header, *metadata_store);
    if (authentication.status == AuthenticationStatus::storage_error) {
        RouteResponse response = rpc_error(
            503, nullptr, -32000, "Authentication storage is unavailable.");
        response.internal_error = authentication.internal_error;
        return response;
    }
    if (authentication.status != AuthenticationStatus::authenticated) {
        RouteResponse response = rpc_error(
            401, nullptr, -32000, "Provide a valid personal Bearer API key.");
        response.www_authenticate = "Bearer realm=\"synsigra_mcp\"";
        return response;
    }
    if (!protocol_version_header.empty() &&
        !supported_protocol(protocol_version_header)) {
        return rpc_error(400, nullptr, -32600, "Unsupported MCP-Protocol-Version header.");
    }
    if (method == "GET" || method == "DELETE") {
        RouteResponse quota = invoke_api(
            "GET", public_base_path + "/v1/usage", "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        if (quota.status != 200) return quota;
        return rpc_error(405, nullptr, -32600,
            "This stateless MCP server does not expose an SSE stream or sessions; use POST.");
    }
    if (method != "POST") {
        return rpc_error(405, nullptr, -32600, "The MCP endpoint accepts POST and GET.");
    }
    if (!accepts_mcp(accept_header)) {
        return rpc_error(406, nullptr, -32600,
            "Accept must include application/json and text/event-stream.");
    }
    if (!json_content_type(content_type)) {
        return rpc_error(415, nullptr, -32600,
            "Content-Type must be application/json.");
    }
    json_error_t parse_error;
    json_t* root = json_loadb(
        request_body.data(), request_body.size(), JSON_REJECT_DUPLICATES,
        &parse_error);
    if (!json_is_object(root)) {
        if (root != nullptr) json_decref(root);
        return rpc_error(400, nullptr, -32700, "Invalid JSON-RPC payload.");
    }
    json_t* jsonrpc = json_object_get(root, "jsonrpc");
    json_t* id = json_object_get(root, "id");
    json_t* method_value = json_object_get(root, "method");
    const bool notification = id == nullptr;
    if (!json_is_string(jsonrpc) ||
        std::string(json_string_value(jsonrpc)) != "2.0" ||
        (!notification && !json_is_string(id) && !json_is_integer(id)) ||
        (!json_is_string(method_value) && !notification)) {
        RouteResponse response = rpc_error(400, id, -32600, "Invalid JSON-RPC request.");
        json_decref(root);
        return response;
    }
    const std::string rpc_method = json_is_string(method_value)
        ? json_string_value(method_value) : std::string();

    if (notification || rpc_method.empty()) {
        RouteResponse quota = invoke_api(
            "GET", public_base_path + "/v1/usage", "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        json_decref(root);
        if (quota.status != 200) return quota;
        RouteResponse accepted;
        accepted.disposition = RouteDisposition::handled;
        accepted.status = 202;
        accepted.content_type = "application/json; charset=utf-8";
        accepted.cache_control = "no-store";
        return accepted;
    }

    if (rpc_method == "initialize") {
        RouteResponse quota = invoke_api(
            "GET", public_base_path + "/v1/usage", "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        if (quota.status != 200) {
            json_decref(root);
            return quota;
        }
        json_t* params = json_object_get(root, "params");
        const std::string requested = string_field(params, "protocolVersion");
        const std::string negotiated = supported_protocol(requested)
            ? requested : kLatestProtocol;
        json_t* result = json_object();
        json_object_set_new(result, "protocolVersion", json_string(negotiated.c_str()));
        json_t* capabilities = json_object();
        json_object_set_new(capabilities, "tools", json_object());
        json_object_set_new(capabilities, "prompts", json_object());
        json_object_set_new(result, "capabilities", capabilities);
        json_t* server = json_object();
        json_object_set_new(server, "name", json_string("synsigra"));
        json_object_set_new(server, "title", json_string("Synsigra Algorithm QA"));
        json_object_set_new(server, "version", json_string("0.1.0"));
        json_object_set_new(server, "description", json_string(
            "Goal-directed synthetic biosignal challenge generation and local verification guidance."));
        json_object_set_new(server, "websiteUrl", json_string(
            absolute_url(email_config, public_base_path + "/mcp-setup").c_str()));
        json_object_set_new(result, "serverInfo", server);
        json_object_set_new(result, "instructions", json_string(
            "Use only synthetic engineering requirements; never send PHI or patient data. Start with synsigra_recommend_packs. Inspect the selected pack and its scoreable/reference-only boundaries before asking for approval. For unmet duration, sampling-rate or target requirements, use the live custom-authoring contract and preview every scenario. Never invent schema fields. Creating/rebuilding jobs consumes quota and should remain human-approved. Poll jobs with restraint. Proprietary algorithms and outputs stay local: download the verification kit, run the algorithm against challenge/, populate submission/, and run synsigra-verify. Never claim clinical validation."));
        RouteResponse response = rpc_result(id, result);
        json_decref(root);
        return response;
    }

    if (rpc_method == "ping") {
        RouteResponse quota = invoke_api(
            "GET", public_base_path + "/v1/usage", "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        if (quota.status != 200) {
            json_decref(root);
            return quota;
        }
        RouteResponse response = rpc_result(id, json_object());
        json_decref(root);
        return response;
    }

    if (rpc_method == "tools/list") {
        RouteResponse quota = invoke_api(
            "GET", public_base_path + "/v1/usage", "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        if (quota.status != 200) {
            json_decref(root);
            return quota;
        }
        json_error_t tools_error;
        json_t* tools = json_loads(kToolsJson, JSON_REJECT_DUPLICATES, &tools_error);
        json_t* result = json_object();
        json_object_set_new(result, "tools", tools == nullptr ? json_array() : tools);
        RouteResponse response = rpc_result(id, result);
        json_decref(root);
        return response;
    }

    if (rpc_method == "prompts/list") {
        RouteResponse quota = invoke_api(
            "GET", public_base_path + "/v1/usage", "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        if (quota.status != 200) {
            json_decref(root);
            return quota;
        }
        json_t* result = json_object();
        json_t* prompts = json_array();
        json_error_t error;
        json_t* design = json_loads(
            "{\"name\":\"design_algorithm_qa_pack\",\"title\":\"Design an algorithm QA pack\",\"description\":\"Turn an engineering goal into an approved, generated Synsigra challenge.\",\"arguments\":[{\"name\":\"goal\",\"description\":\"Algorithm outputs and test requirements\",\"required\":true}]}",
            0, &error);
        json_t* verify = json_loads(
            "{\"name\":\"verify_algorithm_outputs\",\"title\":\"Verify algorithm outputs\",\"description\":\"Guide local submission and scoring for a completed job.\",\"arguments\":[{\"name\":\"job_id\",\"description\":\"Completed Synsigra job ID\",\"required\":true}]}",
            0, &error);
        json_array_append_new(prompts, design);
        json_array_append_new(prompts, verify);
        json_object_set_new(result, "prompts", prompts);
        RouteResponse response = rpc_result(id, result);
        json_decref(root);
        return response;
    }

    if (rpc_method == "prompts/get") {
        RouteResponse quota = invoke_api(
            "GET", public_base_path + "/v1/usage", "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        if (quota.status != 200) {
            json_decref(root);
            return quota;
        }
        json_t* params = json_object_get(root, "params");
        const std::string name = string_field(params, "name");
        json_t* arguments = json_is_object(params)
            ? json_object_get(params, "arguments") : nullptr;
        const std::string value = name == "design_algorithm_qa_pack"
            ? string_field(arguments, "goal") : string_field(arguments, "job_id");
        if ((name != "design_algorithm_qa_pack" &&
             name != "verify_algorithm_outputs") || value.empty()) {
            RouteResponse response = rpc_error(200, id, -32602,
                "Unknown prompt or missing required argument.");
            json_decref(root);
            return response;
        }
        const std::string prompt = name == "design_algorithm_qa_pack"
            ? "Design a Synsigra synthetic engineering-QA challenge for this goal: " + value +
              "\nStart with recommend_packs. Explain inferred targets and scoring boundaries. Use custom authoring if exact requirements are unmet. Ask for approval before create_job, then poll and provide the verification guide. Never send PHI or make clinical claims."
            : "For Synsigra job " + value +
              ", inspect status and provide the exact local verification workflow. Keep the proprietary algorithm and outputs local. Explain challenge/, submission/, synsigra-verify, reports and exit codes.";
        json_t* result = json_object();
        json_object_set_new(result, "description", json_string(
            name == "design_algorithm_qa_pack"
                ? "Goal-directed Synsigra pack workflow"
                : "Local Synsigra verification workflow"));
        json_t* messages = json_array();
        json_t* message = json_object();
        json_object_set_new(message, "role", json_string("user"));
        json_t* content = json_object();
        json_object_set_new(content, "type", json_string("text"));
        json_object_set_new(content, "text", json_string(prompt.c_str()));
        json_object_set_new(message, "content", content);
        json_array_append_new(messages, message);
        json_object_set_new(result, "messages", messages);
        RouteResponse response = rpc_result(id, result);
        json_decref(root);
        return response;
    }

    if (rpc_method != "tools/call") {
        RouteResponse quota = invoke_api(
            "GET", public_base_path + "/v1/usage", "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        if (quota.status != 200) {
            json_decref(root);
            return quota;
        }
        RouteResponse response = rpc_error(200, id, -32601, "Method not found.");
        json_decref(root);
        return response;
    }

    json_t* params = json_object_get(root, "params");
    const std::string tool = string_field(params, "name");
    json_t* arguments = json_is_object(params)
        ? json_object_get(params, "arguments") : nullptr;
    if (tool.empty() || (arguments != nullptr && !json_is_object(arguments))) {
        RouteResponse response = rpc_error(200, id, -32602, "Invalid tool arguments.");
        json_decref(root);
        return response;
    }
    if (arguments == nullptr) arguments = json_object();

    const bool local_tool = tool == "synsigra_recommend_packs" ||
        tool == "synsigra_list_packs";
    if (local_tool || (tool != "synsigra_get_pack" &&
        tool != "synsigra_list_projects" && tool != "synsigra_create_job" &&
        tool != "synsigra_get_job" && tool != "synsigra_list_jobs" &&
        tool != "synsigra_get_verification_guide" &&
        tool != "synsigra_rebuild_expired_job" &&
        tool != "synsigra_get_authoring_contract" &&
        tool != "synsigra_get_curated_scenario" &&
        tool != "synsigra_preview_scenario" &&
        tool != "synsigra_save_scenario" &&
        tool != "synsigra_create_custom_pack")) {
        RouteResponse quota = invoke_api(
            "GET", public_base_path + "/v1/usage", "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        if (quota.status != 200) {
            if (arguments != json_object_get(params, "arguments")) json_decref(arguments);
            json_decref(root);
            return quota;
        }
    }

    RouteResponse response;
    if (tool == "synsigra_recommend_packs") {
        if (string_field(arguments, "goal").empty()) {
            response = rpc_error(200, id, -32602, "goal is required.");
        } else {
            std::string error;
            json_t* result = recommend_packs(pack_root, arguments, error);
            if (result == nullptr) {
                json_t* failure = json_object();
                json_object_set_new(failure, "error", json_string("pack_catalog_unavailable"));
                response = tool_response(id, failure, true);
                json_decref(failure);
                response.internal_error = error;
            } else {
                response = tool_response(id, result, false);
                json_decref(result);
            }
        }
    } else if (tool == "synsigra_list_packs") {
        std::vector<PackSummary> packs;
        std::string error;
        if (!PackCatalog(pack_root).list(packs, error)) {
            json_t* failure = json_object();
            json_object_set_new(failure, "error", json_string("pack_catalog_unavailable"));
            response = tool_response(id, failure, true);
            json_decref(failure);
            response.internal_error = error;
        } else {
            const std::string query = lower(string_field(arguments, "query"));
            const std::string target = string_field(arguments, "target");
            const bool scoreable_only = bool_field(arguments, "scoreable_only", false);
            json_t* result = json_object();
            json_t* values = json_array();
            for (std::vector<PackSummary>::const_iterator it = packs.begin();
                 it != packs.end(); ++it) {
                const std::string searchable = lower(
                    it->pack_id + " " + it->display_name + " " + it->description);
                if (!query.empty() && !contains(searchable, query)) continue;
                if (!target.empty() &&
                    !(scoreable_only ? has_target(it->scoreable_targets, target)
                                    : has_string(it->targets, target))) continue;
                json_array_append_new(values, concise_pack(*it));
            }
            json_object_set_new(result, "packs", values);
            json_object_set_new(result, "count", json_integer(json_array_size(values)));
            response = tool_response(id, result, false);
            json_decref(result);
        }
    } else if (tool == "synsigra_get_verification_guide") {
        const std::string job_id = string_field(arguments, "job_id");
        RouteResponse api = invoke_api(
            "GET", public_base_path + "/v1/jobs/" + job_id, "", "",
            public_base_path, authorization_header, metadata_store, pack_root,
            data_root, signal_synth_cli, email_config);
        if (api.status != 200) {
            response = api_tool_response(id, api);
        } else {
            json_t* job = parsed_api_body(api);
            const std::string status = string_field(job, "status");
            json_t* guide = json_object();
            json_object_set_new(guide, "job_id", json_string(job_id.c_str()));
            json_object_set_new(guide, "status", json_string(status.c_str()));
            json_object_set_new(guide, "job_summary", concise_job_summary(job));
            json_object_set_new(guide, "jobs_ui_url", json_string(
                absolute_url(email_config, public_base_path + "/jobs").c_str()));
            std::string response_text = "Synsigra job " + job_id + " is " + status + ".";
            bool guide_error = false;
            if (status == "succeeded") {
                const std::string artifact_status = string_field(job, "artifact_status");
                if (artifact_status == "expired") {
                    json_object_set_new(guide, "next_action", json_string(
                        "Call synsigra_rebuild_expired_job, then poll the returned new job. The exact historical generator will be used."));
                    response_text += " Rebuild the expired artifact before local verification.";
                } else {
                    json_t* challenge = json_object_get(job, "challenge");
                    json_t* verification = json_is_object(challenge)
                        ? json_object_get(challenge, "verification") : nullptr;
                    const std::string mode = string_field(verification, "mode");
                    const bool valid_mode = mode == "evidence" || mode == "diagnostic";
                    const bool evidence_eligible = bool_field(
                        verification, "evidence_eligible", false);
                    json_object_set_new(
                        guide, "challenge", concise_challenge_summary(challenge));
                    json_object_set_new(
                        guide, "verification_mode", json_string(mode.c_str()));
                    json_object_set_new(
                        guide, "evidence_eligible", json_boolean(evidence_eligible));
                    const std::string kit = string_field(job, "verification_kit_url");
                    const std::string kit_url = absolute_url(email_config, kit);
                    json_t* downloads = json_object();
                    json_object_set_new(
                        downloads, "verification_kit_url", json_string(kit_url.c_str()));
                    json_object_set_new(
                        downloads, "verification_kit_filename",
                        json_string((job_id + "-verification-kit.zip").c_str()));
                    json_object_set_new(guide, "download_auth", json_string(
                        "Download with HTTPS and the same Authorization: Bearer API key. Do not put the key in a query string."));
                    const std::string verifier_version = string_field(
                        challenge, "verifier_version");
                    const std::string metadata_url = absolute_url(
                        email_config, public_base_path + "/v1/downloads/verifier");
                    json_object_set_new(
                        downloads, "verifier_metadata_url",
                        json_string(metadata_url.c_str()));
                    std::string wheel_url;
                    std::string install_command;
                    if (!verifier_version.empty()) {
                        const std::string wheel = "synsigra-" + verifier_version +
                            "-py3-none-any.whl";
                        wheel_url = absolute_url(
                            email_config,
                            public_base_path + "/v1/downloads/verifier/" + wheel);
                        install_command = "python -m pip install " + wheel;
                        json_object_set_new(
                            downloads, "verifier_wheel_url",
                            json_string(wheel_url.c_str()));
                        json_object_set_new(
                            downloads, "verifier_wheel_filename",
                            json_string(wheel.c_str()));
                        json_object_set_new(
                            downloads, "verifier_install_command",
                            json_string(install_command.c_str()));
                    }
                    json_object_set_new(guide, "downloads", downloads);
                    if (!valid_mode) {
                        json_object_set_new(guide, "error", json_string(
                            "The succeeded job has no supported verification mode."));
                        guide_error = true;
                        response_text += " Its verification metadata is invalid; do not infer a command.";
                    } else {
                        const std::string command = std::string(
                            "synsigra-verify challenge submission verification-results") +
                            (mode == "diagnostic" ? " --mode diagnostic" : "") +
                            " --force";
                        json_object_set_new(
                            guide, "verification_command", json_string(command.c_str()));
                        json_object_set_new(
                            guide, "run_from", json_string("verification-kit/"));
                        json_t* steps = json_array();
                        json_array_append_new(steps, json_string(
                            "Download the verification kit and verifier wheel with the same Bearer API key."));
                        json_array_append_new(steps, json_string(
                            "Unzip the kit, run the proprietary algorithm locally against challenge/, and replace only the declared outputs and algorithm identity under submission/."));
                        json_array_append_new(steps, json_string(
                            ("From verification-kit/, run exactly: " + command).c_str()));
                        json_array_append_new(steps, json_string(
                            "Open verification-results/index.html and follow its links; evidence.json is the single canonical machine-readable record."));
                        json_object_set_new(guide, "steps", steps);
                        json_t* result = json_object();
                        json_object_set_new(
                            result, "entrypoint",
                            json_string("verification-results/index.html"));
                        json_object_set_new(
                            result, "canonical_evidence",
                            json_string("verification-results/evidence.json"));
                        json_object_set_new(
                            result, "details_directory",
                            json_string("verification-results/details/"));
                        json_object_set_new(
                            result, "notice",
                            json_string("Synthetic engineering QA evidence; not diagnosis, nor clinical evidence"));
                        json_object_set_new(guide, "result", result);
                        json_t* exit_codes = json_object();
                        json_object_set_new(exit_codes, "pass", json_integer(0));
                        json_object_set_new(
                            exit_codes, "verification_failed", json_integer(1));
                        json_object_set_new(
                            exit_codes, "invalid_cli_usage", json_integer(2));
                        json_object_set_new(guide, "exit_codes", exit_codes);
                        json_t* archive = json_array();
                        json_array_append_new(archive, json_string(
                            "verification-kit.zip or the immutable extracted challenge/"));
                        json_array_append_new(archive, json_string(
                            "completed submission/"));
                        json_array_append_new(archive, json_string(
                            "verification-results/ including index.html, evidence.json and details/"));
                        json_array_append_new(archive, json_string(
                            "algorithm build identity, version and configuration"));
                        json_array_append_new(archive, json_string(
                            "Synsigra job ID and generator commit from job_summary"));
                        json_object_set_new(guide, "archive_checklist", archive);
                        json_object_set_new(guide, "privacy_boundary", json_string(
                            "Algorithm binaries and outputs remain local and are not uploaded to Synsigra."));
                        response_text = "Synsigra job " + job_id + " is ready for " +
                            mode + " verification. Download the kit" +
                            (wheel_url.empty() ? std::string() : " and verifier wheel") +
                            ", run from verification-kit/: " + command +
                            ". Open verification-results/index.html.";
                    }
                }
            } else if (status == "failed" || status == "cancelled") {
                json_object_set_new(guide, "next_action", json_string(
                    "Inspect the returned job error. Do not claim a package was generated."));
            } else {
                json_object_set_new(guide, "next_action", json_string(
                    "Poll synsigra_get_job with backoff until succeeded, failed or cancelled."));
            }
            response = tool_response(id, guide, guide_error, response_text);
            json_decref(guide);
            json_decref(job);
        }
    } else {
        std::string api_method = "GET";
        std::string api_uri;
        std::string api_query;
        std::string api_body;
        if (tool == "synsigra_get_pack") {
            api_uri = public_base_path + "/v1/packs/" + string_field(arguments, "pack_id");
        } else if (tool == "synsigra_list_projects") {
            api_uri = public_base_path + "/v1/projects";
        } else if (tool == "synsigra_create_job") {
            api_method = "POST";
            api_uri = public_base_path + "/v1/jobs";
            api_body = request_body_from(copy_argument_object(arguments));
        } else if (tool == "synsigra_get_job") {
            api_uri = public_base_path + "/v1/jobs/" + string_field(arguments, "job_id");
        } else if (tool == "synsigra_list_jobs") {
            api_uri = public_base_path + "/v1/jobs";
            api_query = "limit=" + std::to_string(integer_field(arguments, "limit", 25)) +
                "&offset=" + std::to_string(integer_field(arguments, "offset", 0));
        } else if (tool == "synsigra_rebuild_expired_job") {
            api_method = "POST";
            api_uri = public_base_path + "/v1/jobs/" +
                string_field(arguments, "job_id") + "/rebuild";
        } else if (tool == "synsigra_get_authoring_contract") {
            api_uri = public_base_path + "/v1/authoring/" +
                string_field(arguments, "document");
        } else if (tool == "synsigra_get_curated_scenario") {
            api_uri = public_base_path + "/v1/authoring/curated-scenarios/" +
                string_field(arguments, "pack_id") + "/" +
                string_field(arguments, "case_id");
        } else if (tool == "synsigra_preview_scenario") {
            api_method = "POST";
            api_uri = public_base_path + "/v1/authoring/preview";
            api_body = request_body_from(copy_argument_object(arguments));
        } else if (tool == "synsigra_save_scenario") {
            api_method = "POST";
            api_uri = public_base_path + "/v1/scenarios";
            api_body = request_body_from(copy_argument_object(arguments));
        } else if (tool == "synsigra_create_custom_pack") {
            api_method = "POST";
            api_uri = public_base_path + "/v1/custom-packs";
            api_body = request_body_from(copy_argument_object(arguments));
        } else {
            response = rpc_error(200, id, -32602, "Unknown tool.");
        }
        if (!api_uri.empty()) {
            response = api_tool_response(id, invoke_api(
                api_method, api_uri, api_query, api_body, public_base_path,
                authorization_header, metadata_store, pack_root, data_root,
                signal_synth_cli, email_config));
        }
    }

    if (arguments != json_object_get(params, "arguments")) json_decref(arguments);
    json_decref(root);
    return response;
}

}  // namespace syn_sig_ra
