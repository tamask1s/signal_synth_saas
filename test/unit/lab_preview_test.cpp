#include "syn_sig_ra/lab_preview.h"
#include "syn_sig_ra/scenario_schema.h"
#include "syn_sig_ra/signal_viewer.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool remove_tree(const std::string& path) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) return true;
    if (S_ISDIR(information.st_mode)) {
        DIR* handle = opendir(path.c_str());
        if (handle == nullptr) return false;
        bool succeeded = true;
        for (dirent* entry = readdir(handle); entry != nullptr; entry = readdir(handle)) {
            const std::string name(entry->d_name);
            if (name != "." && name != ".." && !remove_tree(path + "/" + name)) {
                succeeded = false;
                break;
            }
        }
        closedir(handle);
        return succeeded && rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

}  // namespace

int main() {
    std::ostringstream path;
    path << "/tmp/syn_sig_ra_lab_preview_test_" << getpid();
    const std::string root = path.str();
    require(mkdir(root.c_str(), 0700) == 0, "test data root should be created");

    syn_sig_ra::ApiKeyIdentity owner;
    owner.organization_id = "org_lab_test";
    owner.user_id = "user_lab_test";
    owner.role = "developer";
    const std::string old_scenario =
        "{\"schema_version\":2,\"scenario_id\":\"lab_case\","
        "\"name\":\"Lab case\",\"description\":\"Preview test\","
        "\"author\":\"Synsigra\",\"tags\":[\"lab\"],"
        "\"duration_seconds\":2,\"sample_rate_hz\":100,\"seed\":12345,"
        "\"ecg\":{\"heart_rate_bpm\":70,\"rr_variability_seconds\":0,"
        "\"ectopic_every_n_beats\":0,"
        "\"second_degree_av_pattern\":\"unspecified\","
        "\"q_wave_territory\":\"unspecified\",\"rhythm_episodes\":[],"
        "\"flutter_conduction_pattern\":\"fixed\","
        "\"pacing_mode\":\"ventricular\","
        "\"pacing_non_capture_every_n_beats\":0,"
        "\"fidelity_policy\":\"allow_parameterized\","
        "\"conditions\":[{\"code\":\"NORM\",\"severity\":1}]},"
        "\"ppg\":{\"enabled\":true,\"pulse_delay_ms\":180,"
        "\"rise_time_ms\":120,\"decay_time_ms\":300,"
        "\"amplitude_au\":1,\"baseline_au\":0,"
        "\"dicrotic_delay_ms\":180,\"dicrotic_width_ms\":80,"
        "\"dicrotic_amplitude_ratio\":0.15}}";
    std::string scenario;
    std::vector<std::string> normalization_messages;
    require(
        syn_sig_ra::normalize_current_scenario_json(
            old_scenario, scenario, normalization_messages),
        "Lab fixture should normalize to current schema"
    );
    syn_sig_ra::LabPreview preview;
    std::vector<std::string> validation;
    std::string error;
    require(
        syn_sig_ra::create_lab_preview(
            root, owner, scenario, preview, validation, error) ==
            syn_sig_ra::LabPreviewStatus::ok,
        "valid Lab case should render: " + error);
    require(
        preview.preview_id.compare(0, 8, "preview_") == 0 &&
            preview.sample_count == 200u && preview.sample_rate_hz == 100u &&
            !preview.document_fingerprint.empty() &&
            !preview.resolved_scenario_json.empty(),
        "preview should expose exact identity and bounded render metadata");

    syn_sig_ra::LabPreview loaded;
    std::string viewer_root;
    require(
        syn_sig_ra::load_lab_preview(
            root, owner, preview.preview_id, loaded, viewer_root, error) ==
            syn_sig_ra::LabPreviewStatus::ok &&
            loaded.document_fingerprint == preview.document_fingerprint,
        "owner should reload exact preview metadata: " + error);
    syn_sig_ra::SignalViewerSource source;
    require(
        syn_sig_ra::describe_signal_viewer_source(
            viewer_root, source, error) == syn_sig_ra::SignalViewerStatus::ok &&
            source.cases.size() == 1u &&
            source.cases[0].channels.size() >= 13u &&
            source.cases[0].channels[12].name.find("ppg") != std::string::npos,
        "prepared preview should expose ECG and PPG channels: " + error);
    syn_sig_ra::SignalViewerWindowRequest request;
    request.case_id = "preview";
    request.start_sample = 0;
    request.sample_count = 200;
    request.max_points = 200;
    request.channel_indices.push_back(1);
    request.channel_indices.push_back(12);
    syn_sig_ra::SignalViewerWindow window;
    require(
        syn_sig_ra::read_signal_viewer_window(
            viewer_root, request, window, error) ==
            syn_sig_ra::SignalViewerStatus::ok &&
            window.bucket_count == 200u && window.channel_indices.size() == 2u &&
            !window.binary.empty(),
        "preview viewer should return bounded binary ECG/PPG data: " + error);

    syn_sig_ra::ApiKeyIdentity other = owner;
    other.user_id = "other_user";
    require(
        syn_sig_ra::load_lab_preview(
            root, other, preview.preview_id, loaded, viewer_root, error) ==
            syn_sig_ra::LabPreviewStatus::not_found,
        "another user must not resolve the preview");

    syn_sig_ra::ApiKeyIdentity rhythm_owner = owner;
    rhythm_owner.user_id = "user_lab_rhythm";
    std::string rhythm_scenario = scenario;
    const std::string empty_episodes = "\"rhythm_episodes\":[]";
    const std::string timed_episode =
        "\"rhythm_episodes\":[{\"type\":\"afib\",\"start_seconds\":0.3,"
        "\"duration_seconds\":1.4,\"transition_seconds\":0.1,"
        "\"rate_bpm\":120,\"seed\":12386}]";
    rhythm_scenario.replace(
        rhythm_scenario.find(empty_episodes), empty_episodes.size(),
        timed_episode);
    const std::string normal_condition =
        "\"conditions\":[{\"code\":\"NORM\",\"severity\":1}]";
    const std::string sinus_condition =
        "\"conditions\":[{\"code\":\"SR\",\"severity\":1}]";
    rhythm_scenario.replace(
        rhythm_scenario.find(normal_condition), normal_condition.size(),
        sinus_condition);
    syn_sig_ra::LabPreview rhythm_preview;
    require(
        syn_sig_ra::create_lab_preview(
            root, rhythm_owner, rhythm_scenario, rhythm_preview,
            validation, error) == syn_sig_ra::LabPreviewStatus::ok &&
            rhythm_preview.sample_count == 200u,
        "timed rhythm episode from the human Lab controls should render: " +
            error);
    require(
        syn_sig_ra::discard_lab_preview(
            root, rhythm_owner, rhythm_preview.preview_id, error) ==
            syn_sig_ra::LabPreviewStatus::ok,
        "rhythm preview should be discarded: " + error);
    require(
        syn_sig_ra::discard_lab_preview(
            root, owner, preview.preview_id, error) ==
            syn_sig_ra::LabPreviewStatus::ok,
        "owner should discard preview: " + error);
    require(
        syn_sig_ra::load_lab_preview(
            root, owner, preview.preview_id, loaded, viewer_root, error) ==
            syn_sig_ra::LabPreviewStatus::not_found,
        "discarded preview should be gone");
    require(remove_tree(root), "test data root should be removed");
    std::cout << "lab preview tests passed\n";
    return EXIT_SUCCESS;
}
