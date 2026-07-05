#include "syn_sig_ra/job_request.h"

#include "syn_sig_ra/pack_catalog.h"

#include <jansson.h>

#include <set>
#include <string>

namespace syn_sig_ra {

JobRequestStatus parse_job_request(
    const std::string& body,
    JobRequest& request,
    std::string& error
) {
    if (body.empty() || body.size() > 64u * 1024u) {
        error = "request body must contain at most 64 KiB of JSON";
        return JobRequestStatus::invalid_json;
    }

    json_error_t parse_error;
    json_t* root = json_loadb(
        body.data(),
        body.size(),
        JSON_REJECT_DUPLICATES,
        &parse_error
    );
    if (root == nullptr) {
        error = parse_error.text;
        return JobRequestStatus::invalid_json;
    }
    if (!json_is_object(root)) {
        json_decref(root);
        error = "request root must be a JSON object";
        return JobRequestStatus::invalid_shape;
    }

    const std::set<std::string> allowed_fields = {
        "project_id",
        "pack_id"
    };
    const char* field_name = nullptr;
    json_t* field_value = nullptr;
    json_object_foreach(root, field_name, field_value) {
        if (allowed_fields.count(field_name) == 0) {
            error = std::string("unsupported request field: ") + field_name;
            json_decref(root);
            return JobRequestStatus::unsupported_field;
        }
    }

    json_t* project_id = json_object_get(root, "project_id");
    if (!json_is_string(project_id) ||
        !is_valid_pack_id(json_string_value(project_id))) {
        json_decref(root);
        error = "project_id must be a safe project identifier";
        return JobRequestStatus::invalid_value;
    }
    json_t* pack_id = json_object_get(root, "pack_id");
    if (!json_is_string(pack_id) ||
        !is_valid_pack_id(json_string_value(pack_id))) {
        json_decref(root);
        error = "pack_id must be a safe catalog identifier";
        return JobRequestStatus::invalid_value;
    }

    JobRequest parsed;
    parsed.project_id = json_string_value(project_id);
    parsed.pack_id = json_string_value(pack_id);

    char* canonical = json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS);
    json_decref(root);
    if (canonical == nullptr) {
        error = "unable to canonicalize request JSON";
        return JobRequestStatus::invalid_json;
    }
    parsed.canonical_json = canonical;
    free(canonical);
    request = parsed;
    return JobRequestStatus::valid;
}

}  // namespace syn_sig_ra
