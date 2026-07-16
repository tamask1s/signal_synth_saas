#include "syn_sig_ra/signal_viewer.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
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

void write_i16(std::ofstream& output, short value) {
    const unsigned short encoded = static_cast<unsigned short>(value);
    const char bytes[2] = {
        static_cast<char>(encoded & 0xffu),
        static_cast<char>((encoded >> 8u) & 0xffu)
    };
    output.write(bytes, 2);
}

unsigned int u32(const std::string& value, std::size_t offset) {
    return static_cast<unsigned int>(
        static_cast<unsigned char>(value[offset]) |
        (static_cast<unsigned int>(static_cast<unsigned char>(value[offset + 1u])) << 8u) |
        (static_cast<unsigned int>(static_cast<unsigned char>(value[offset + 2u])) << 16u) |
        (static_cast<unsigned int>(static_cast<unsigned char>(value[offset + 3u])) << 24u)
    );
}

unsigned long long u64(const std::string& value, std::size_t offset) {
    unsigned long long result = 0;
    for (unsigned int index = 0; index < 8u; ++index) {
        result |= static_cast<unsigned long long>(
            static_cast<unsigned char>(value[offset + index])) << (index * 8u);
    }
    return result;
}

short i16(const std::string& value, std::size_t offset) {
    return static_cast<short>(
        static_cast<unsigned int>(static_cast<unsigned char>(value[offset])) |
        (static_cast<unsigned int>(static_cast<unsigned char>(value[offset + 1u])) << 8u)
    );
}

bool remove_tree(const std::string& path) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) return true;
    if (S_ISDIR(information.st_mode)) {
        DIR* handle = opendir(path.c_str());
        if (handle == nullptr) return false;
        bool succeeded = true;
        for (dirent* entry = readdir(handle);
             entry != nullptr; entry = readdir(handle)) {
            const std::string name(entry->d_name);
            if (name != "." && name != ".." &&
                !remove_tree(path + "/" + name)) {
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
    std::ostringstream root_stream;
    root_stream << "/tmp/syn_sig_ra_signal_viewer_test_" << getpid();
    const std::string root = root_stream.str();
    const std::string extracted = root + "/extracted";
    const std::string cases = extracted + "/cases";
    const std::string source_case = cases + "/large_case";
    const std::string viewer = root + "/viewer";
    require(mkdir(root.c_str(), 0700) == 0, "test root should be created");
    require(mkdir(extracted.c_str(), 0700) == 0, "extracted root should be created");
    require(mkdir(cases.c_str(), 0700) == 0, "case root should be created");
    require(mkdir(source_case.c_str(), 0700) == 0, "source case should be created");

    const unsigned long long sample_count = 200003u;
    {
        std::ofstream header((source_case + "/synsigra.hea").c_str());
        header << "synsigra 3 500 " << sample_count << '\n'
               << "# generator=signal_synth test\n"
               << "# scenario_id=large_case\n"
               << "synsigra.dat 16 1000(0)/mV 16 0 0 0 0 II\n"
               << "synsigra.dat 16 10000(0)/NU 16 0 0 0 0 ppg_green\n"
               << "synsigra.dat 16 10000(0)/g 16 0 0 0 0 accel_motion\n";
        require(header.good(), "WFDB header fixture should be written");
    }
    {
        std::ofstream data(
            (source_case + "/synsigra.dat").c_str(),
            std::ios::binary | std::ios::trunc
        );
        for (unsigned long long sample = 0; sample < sample_count; ++sample) {
            write_i16(data, static_cast<short>(sample % 200u) - 100);
            write_i16(data, static_cast<short>(sample % 1000u) - 500);
            write_i16(data, sample == 123456u
                ? static_cast<short>(30000)
                : static_cast<short>(-200));
        }
        require(data.good(), "WFDB data fixture should be written");
    }

    std::string error;
    require(
        syn_sig_ra::prepare_signal_viewer_source(extracted, viewer, error) ==
            syn_sig_ra::SignalViewerStatus::ok,
        "viewer source should be prepared: " + error
    );
    syn_sig_ra::SignalViewerSource description;
    require(
        syn_sig_ra::describe_signal_viewer_source(viewer, description, error) ==
            syn_sig_ra::SignalViewerStatus::ok &&
            description.cases.size() == 1u &&
            description.cases[0].sample_count == sample_count &&
            description.cases[0].sample_rate_hz == 500.0 &&
            description.cases[0].channels.size() == 3u &&
            description.cases[0].channels[1].name == "ppg_green",
        "viewer metadata should describe the WFDB source: " + error
    );

    syn_sig_ra::SignalViewerWindowRequest raw_request;
    raw_request.case_id = "large_case";
    raw_request.start_sample = 10u;
    raw_request.sample_count = 8u;
    raw_request.max_points = 8u;
    raw_request.channel_indices.push_back(1u);
    syn_sig_ra::SignalViewerWindow raw;
    require(
        syn_sig_ra::read_signal_viewer_window(
            viewer, raw_request, raw, error) == syn_sig_ra::SignalViewerStatus::ok,
        "raw viewport should be read: " + error
    );
    require(
        raw.binary.compare(0, 8, "SYNSIGV1") == 0 &&
            u32(raw.binary, 16u) == 1u &&
            u32(raw.binary, 20u) == 8u &&
            u64(raw.binary, 24u) == 10u &&
            u64(raw.binary, 48u) == 1u &&
            u32(raw.binary, 80u) == 1u,
        "binary viewport header should be stable"
    );
    const std::size_t raw_payload = u32(raw.binary, 72u);
    require(
        i16(raw.binary, raw_payload) == -490 &&
            i16(raw.binary, raw_payload + 2u) == -490 &&
            i16(raw.binary, raw_payload + 28u) == -483,
        "raw viewport should contain physical-channel digital samples"
    );

    syn_sig_ra::SignalViewerWindowRequest overview_request;
    overview_request.case_id = "large_case";
    overview_request.start_sample = 123000u;
    overview_request.sample_count = 2000u;
    overview_request.max_points = 10u;
    overview_request.channel_indices.push_back(2u);
    syn_sig_ra::SignalViewerWindow overview;
    require(
        syn_sig_ra::read_signal_viewer_window(
            viewer, overview_request, overview, error) ==
                syn_sig_ra::SignalViewerStatus::ok &&
            overview.samples_per_bucket == 256u &&
            overview.bucket_count <= 12u &&
            overview.binary.size() == 84u + overview.bucket_count * 4u,
        "pyramid viewport should remain bounded: " + error
    );
    bool found_spike = false;
    const std::size_t overview_payload = u32(overview.binary, 72u);
    for (unsigned int bucket = 0; bucket < overview.bucket_count; ++bucket) {
        if (i16(overview.binary, overview_payload + bucket * 4u + 2u) == 30000) {
            found_spike = true;
        }
    }
    require(found_spike, "pyramid viewport should preserve extrema");

    syn_sig_ra::SignalViewerWindowRequest full_request;
    full_request.case_id = "large_case";
    full_request.start_sample = 0u;
    full_request.sample_count = sample_count;
    full_request.max_points = 128u;
    full_request.channel_indices.push_back(0u);
    full_request.channel_indices.push_back(1u);
    syn_sig_ra::SignalViewerWindow full;
    require(
        syn_sig_ra::read_signal_viewer_window(
            viewer, full_request, full, error) ==
                syn_sig_ra::SignalViewerStatus::ok &&
            full.bucket_count <= 130u &&
            full.binary.size() <= 80u + 8u + 130u * 8u,
        "full-record response should be independent of source file size"
    );

    syn_sig_ra::SignalViewerWindowRequest invalid = raw_request;
    invalid.channel_indices.push_back(1u);
    require(
        syn_sig_ra::read_signal_viewer_window(
            viewer, invalid, full, error) ==
            syn_sig_ra::SignalViewerStatus::invalid_request,
        "duplicate channels should be rejected"
    );

    require(remove_tree(root), "viewer fixture should be removed");
    return EXIT_SUCCESS;
}
