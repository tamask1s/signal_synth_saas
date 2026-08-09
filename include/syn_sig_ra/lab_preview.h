#ifndef SYN_SIG_RA_LAB_PREVIEW_H
#define SYN_SIG_RA_LAB_PREVIEW_H

#include <string>
#include <vector>

#include "syn_sig_ra/metadata_store.h"

namespace syn_sig_ra {

enum class LabPreviewStatus {
    ok,
    invalid_request,
    invalid_scenario,
    limit_exceeded,
    busy,
    not_found,
    io_error
};

struct LabPreview {
    std::string preview_id;
    std::string case_id;
    std::string canonical_scenario_json;
    std::string resolved_scenario_json;
    std::string document_fingerprint;
    std::string resolved_document_fingerprint;
    std::string render_identity;
    std::string generator_version;
    std::string generator_git_commit;
    std::string generator_build_identity;
    std::string created_at;
    std::string expires_at;
    double duration_seconds = 0.0;
    unsigned int sample_rate_hz = 0;
    unsigned long long sample_count = 0;
};

// Lab previews are deliberately short-lived and filesystem-only. They do not
// create jobs, packages, or database records.
LabPreviewStatus create_lab_preview(
    const std::string& data_root,
    const ApiKeyIdentity& owner,
    const std::string& scenario_json,
    LabPreview& preview,
    std::vector<std::string>& validation_messages,
    std::string& error
);

LabPreviewStatus load_lab_preview(
    const std::string& data_root,
    const ApiKeyIdentity& owner,
    const std::string& preview_id,
    LabPreview& preview,
    std::string& viewer_root,
    std::string& error
);

LabPreviewStatus discard_lab_preview(
    const std::string& data_root,
    const ApiKeyIdentity& owner,
    const std::string& preview_id,
    std::string& error
);

bool discard_lab_previews_for_user(
    const std::string& data_root,
    const ApiKeyIdentity& owner,
    std::string& error
);

// Safe to call repeatedly from the worker loop. Only expired preview_* trees
// below the dedicated Lab root are removed.
bool cleanup_expired_lab_previews(
    const std::string& data_root,
    std::string& error
);

unsigned int lab_preview_ttl_seconds();
unsigned int lab_preview_maximum_duration_seconds();
unsigned long long lab_preview_maximum_samples();

}  // namespace syn_sig_ra

#endif
