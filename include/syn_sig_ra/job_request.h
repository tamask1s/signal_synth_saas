#ifndef SYN_SIG_RA_JOB_REQUEST_H
#define SYN_SIG_RA_JOB_REQUEST_H

#include <string>
#include <vector>

namespace syn_sig_ra {

struct JobRequest {
    std::string project_id;
    std::string pack_id;
    std::vector<std::string> export_formats;
    std::string report_format;
    std::string canonical_json;
};

enum class JobRequestStatus {
    valid,
    invalid_json,
    invalid_shape,
    unsupported_field,
    invalid_value
};

JobRequestStatus parse_job_request(
    const std::string& body,
    JobRequest& request,
    std::string& error
);

}  // namespace syn_sig_ra

#endif
