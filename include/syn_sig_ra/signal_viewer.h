#ifndef SYN_SIG_RA_SIGNAL_VIEWER_H
#define SYN_SIG_RA_SIGNAL_VIEWER_H

#include <string>
#include <vector>

namespace syn_sig_ra {

enum class SignalViewerStatus {
    ok,
    not_found,
    invalid_request,
    invalid_source,
    io_error
};

struct SignalViewerChannel {
    unsigned int index;
    std::string name;
    std::string unit;
    double gain;
    int adc_zero;
};

struct SignalViewerCase {
    std::string case_id;
    double sample_rate_hz;
    unsigned long long sample_count;
    std::vector<SignalViewerChannel> channels;
};

struct SignalViewerSource {
    std::vector<SignalViewerCase> cases;
};

struct SignalViewerWindowRequest {
    std::string case_id;
    unsigned long long start_sample;
    unsigned long long sample_count;
    unsigned int max_points;
    std::vector<unsigned int> channel_indices;
};

struct SignalViewerWindow {
    unsigned long long requested_start_sample;
    unsigned long long requested_sample_count;
    unsigned long long data_start_sample;
    unsigned long long samples_per_bucket;
    unsigned long long source_sample_count;
    double sample_rate_hz;
    unsigned int bucket_count;
    std::vector<unsigned int> channel_indices;
    std::string binary;
};

struct SignalViewerOverlayRequest {
    std::string case_id;
    unsigned long long start_sample;
    unsigned long long sample_count;
    unsigned int max_items;
};

struct SignalViewerOverlayItem {
    std::string kind;
    std::string source;
    std::string label;
    std::string channel;
    unsigned long long start_sample;
    unsigned long long end_sample;
    bool interval;
    bool has_value;
    double value;
    unsigned int count;
};

struct SignalViewerOverlayWindow {
    unsigned long long requested_start_sample;
    unsigned long long requested_sample_count;
    unsigned long long source_sample_count;
    double sample_rate_hz;
    unsigned long long total_matching_items;
    bool aggregated;
    std::vector<std::string> available_kinds;
    std::vector<SignalViewerOverlayItem> items;
};

// Build a portable, random-access viewer source from an extracted Synsigra
// package. The output contains only WFDB signal data and compact min/max
// pyramid levels. `not_found` means the package has no viewable WFDB cases.
SignalViewerStatus prepare_signal_viewer_source(
    const std::string& extracted_package_root,
    const std::string& viewer_root,
    std::string& error
);

// Inspect a prepared viewer source. This interface intentionally depends only
// on a filesystem root so the same reader can be reused by a local desktop
// server without the SaaS job/database layer.
SignalViewerStatus describe_signal_viewer_source(
    const std::string& viewer_root,
    SignalViewerSource& source,
    std::string& error
);

// Read and encode one viewport. The response is bounded by max_points and the
// requested channel count, independently of the source file size.
SignalViewerStatus read_signal_viewer_window(
    const std::string& viewer_root,
    const SignalViewerWindowRequest& request,
    SignalViewerWindow& window,
    std::string& error
);

// Read ground-truth events and intervals prepared from the generator's
// authoritative annotations.json. Results are time-window bounded; dense
// event streams are aggregated instead of returning an unbounded document.
SignalViewerStatus read_signal_viewer_overlays(
    const std::string& viewer_root,
    const SignalViewerOverlayRequest& request,
    SignalViewerOverlayWindow& window,
    std::string& error
);

const char* signal_viewer_binary_content_type();

}  // namespace syn_sig_ra

#endif
