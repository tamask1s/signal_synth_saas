#include "syn_sig_ra/signal_viewer.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

const unsigned long long kPyramidBaseBlock = 64u;
const unsigned int kMaximumChannels = 64u;
const unsigned int kMaximumPoints = 16384u;

struct ParsedWfdb {
    std::string data_file;
    double sample_rate_hz;
    unsigned long long sample_count;
    std::vector<syn_sig_ra::SignalViewerChannel> channels;
};

bool safe_component(const std::string& value) {
    if (value.empty() || value.size() > 128u || value == "." || value == "..") {
        return false;
    }
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        const unsigned char character = static_cast<unsigned char>(*it);
        if (!(std::isalnum(character) || *it == '_' || *it == '-' || *it == '.')) {
            return false;
        }
    }
    return true;
}

bool regular_file(const std::string& path, long long* size = nullptr) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0 ||
        !S_ISREG(information.st_mode)) {
        return false;
    }
    if (size != nullptr) *size = static_cast<long long>(information.st_size);
    return true;
}

bool directory(const std::string& path) {
    struct stat information;
    return lstat(path.c_str(), &information) == 0 && S_ISDIR(information.st_mode);
}

std::string trim(const std::string& value) {
    const std::string whitespace(" \t\r\n");
    const std::string::size_type first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) return "";
    const std::string::size_type last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1u);
}

bool parse_double_prefix(const std::string& value, double& parsed) {
    char* end = nullptr;
    errno = 0;
    parsed = std::strtod(value.c_str(), &end);
    return errno == 0 && end != value.c_str() && std::isfinite(parsed) && parsed > 0.0;
}

bool parse_wfdb_header(
    const std::string& header_path,
    const std::string& data_path,
    ParsedWfdb& parsed,
    std::string& error
) {
    std::ifstream input(header_path.c_str());
    if (!input) {
        error = "unable to open WFDB header";
        return false;
    }
    std::string first_line;
    if (!std::getline(input, first_line)) {
        error = "WFDB header is empty";
        return false;
    }
    std::istringstream first(first_line);
    std::string record_name;
    std::string sample_rate_token;
    unsigned int channel_count = 0;
    unsigned long long sample_count = 0;
    if (!(first >> record_name >> channel_count >> sample_rate_token >> sample_count) ||
        !safe_component(record_name) || channel_count == 0u ||
        channel_count > kMaximumChannels || sample_count == 0u) {
        error = "WFDB record header is invalid";
        return false;
    }
    double sample_rate_hz = 0.0;
    if (!parse_double_prefix(sample_rate_token, sample_rate_hz)) {
        error = "WFDB sample rate is invalid";
        return false;
    }

    ParsedWfdb fresh;
    fresh.sample_rate_hz = sample_rate_hz;
    fresh.sample_count = sample_count;
    for (unsigned int index = 0; index < channel_count; ++index) {
        std::string line;
        for (;;) {
            if (!std::getline(input, line)) {
                error = "WFDB channel header is incomplete";
                return false;
            }
            const std::string content = trim(line);
            if (!content.empty() && content[0] != '#') break;
        }
        std::istringstream channel_line(line);
        std::string data_file;
        std::string format;
        std::string gain_token;
        std::string adc_resolution;
        int adc_zero = 0;
        std::string initial_value;
        std::string checksum;
        std::string block_size;
        if (!(channel_line >> data_file >> format >> gain_token >> adc_resolution >>
              adc_zero >> initial_value >> checksum >> block_size) ||
            !safe_component(data_file) || format.compare(0, 2, "16") != 0) {
            error = "WFDB channel format is unsupported";
            return false;
        }
        if (fresh.data_file.empty()) fresh.data_file = data_file;
        if (fresh.data_file != data_file) {
            error = "WFDB split signal files are unsupported";
            return false;
        }
        const std::string::size_type parenthesis = gain_token.find('(');
        const std::string::size_type slash = gain_token.find('/');
        const std::string::size_type gain_end = std::min(
            parenthesis == std::string::npos ? gain_token.size() : parenthesis,
            slash == std::string::npos ? gain_token.size() : slash
        );
        double gain = 0.0;
        if (!parse_double_prefix(gain_token.substr(0, gain_end), gain)) {
            error = "WFDB channel gain is invalid";
            return false;
        }
        std::string label;
        std::getline(channel_line, label);
        label = trim(label);
        if (label.empty()) {
            std::ostringstream generated;
            generated << "channel_" << index;
            label = generated.str();
        }
        syn_sig_ra::SignalViewerChannel channel;
        channel.index = index;
        channel.name = label;
        channel.unit = slash == std::string::npos
            ? "digital"
            : gain_token.substr(slash + 1u);
        if (channel.unit.empty()) channel.unit = "digital";
        channel.gain = gain;
        channel.adc_zero = adc_zero;
        fresh.channels.push_back(channel);
    }
    long long data_size = 0;
    if (!regular_file(data_path, &data_size)) {
        error = "WFDB signal file is unavailable";
        return false;
    }
    const unsigned long long frame_bytes =
        static_cast<unsigned long long>(channel_count) * 2u;
    if (sample_count > std::numeric_limits<unsigned long long>::max() / frame_bytes ||
        static_cast<unsigned long long>(data_size) < sample_count * frame_bytes) {
        error = "WFDB signal file is truncated";
        return false;
    }
    parsed = fresh;
    return true;
}

void append_u16(std::string& output, unsigned int value) {
    output.push_back(static_cast<char>(value & 0xffu));
    output.push_back(static_cast<char>((value >> 8u) & 0xffu));
}

void append_i16(std::string& output, short value) {
    append_u16(output, static_cast<unsigned short>(value));
}

void append_u32(std::string& output, unsigned int value) {
    for (unsigned int shift = 0; shift < 32u; shift += 8u) {
        output.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void append_u64(std::string& output, unsigned long long value) {
    for (unsigned int shift = 0; shift < 64u; shift += 8u) {
        output.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void append_f64(std::string& output, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u64(output, static_cast<unsigned long long>(bits));
}

short decode_i16(const char* data) {
    const unsigned int low = static_cast<unsigned char>(data[0]);
    const unsigned int high = static_cast<unsigned char>(data[1]);
    return static_cast<short>(low | (high << 8u));
}

bool copy_file(
    const std::string& source,
    const std::string& destination,
    std::string& error
) {
    std::ifstream input(source.c_str(), std::ios::binary);
    std::ofstream output(destination.c_str(), std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        error = "unable to create viewer signal source";
        return false;
    }
    char buffer[64u * 1024u];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0) output.write(buffer, count);
    }
    if ((!input.eof() && input.fail()) || !output) {
        error = "unable to copy complete viewer signal source";
        return false;
    }
    return true;
}

bool remove_tree(const std::string& path) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0) return errno == ENOENT;
    if (S_ISLNK(information.st_mode)) return false;
    if (S_ISDIR(information.st_mode)) {
        DIR* directory_handle = opendir(path.c_str());
        if (directory_handle == nullptr) return false;
        bool succeeded = true;
        for (dirent* entry = readdir(directory_handle);
             entry != nullptr; entry = readdir(directory_handle)) {
            const std::string name(entry->d_name);
            if (name != "." && name != ".." &&
                !remove_tree(path + "/" + name)) {
                succeeded = false;
                break;
            }
        }
        closedir(directory_handle);
        return succeeded && rmdir(path.c_str()) == 0;
    }
    return S_ISREG(information.st_mode) && unlink(path.c_str()) == 0;
}

std::string pyramid_path(
    const std::string& case_root,
    unsigned long long block_size
) {
    std::ostringstream path;
    path << case_root << "/pyramid_" << block_size << ".bin";
    return path.str();
}

bool build_base_pyramid(
    const std::string& data_path,
    const std::string& output_path,
    const ParsedWfdb& metadata,
    unsigned long long& block_count,
    std::string& error
) {
    std::ifstream input(data_path.c_str(), std::ios::binary);
    std::ofstream output(output_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        error = "unable to open viewer pyramid files";
        return false;
    }
    const std::size_t channels = metadata.channels.size();
    const std::size_t frame_bytes = channels * 2u;
    const std::size_t maximum_bytes =
        static_cast<std::size_t>(kPyramidBaseBlock) * frame_bytes;
    std::vector<char> buffer(maximum_bytes);
    std::vector<short> minimum(channels);
    std::vector<short> maximum(channels);
    block_count = (metadata.sample_count + kPyramidBaseBlock - 1u) /
        kPyramidBaseBlock;
    unsigned long long remaining = metadata.sample_count;
    for (unsigned long long block = 0; block < block_count; ++block) {
        const unsigned long long samples = std::min(
            remaining, kPyramidBaseBlock);
        const std::size_t bytes = static_cast<std::size_t>(samples) * frame_bytes;
        input.read(&buffer[0], static_cast<std::streamsize>(bytes));
        if (input.gcount() != static_cast<std::streamsize>(bytes)) {
            error = "unable to read complete WFDB signal for viewer pyramid";
            return false;
        }
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const short value = decode_i16(&buffer[channel * 2u]);
            minimum[channel] = value;
            maximum[channel] = value;
        }
        for (unsigned long long sample = 1; sample < samples; ++sample) {
            const std::size_t frame = static_cast<std::size_t>(sample) * frame_bytes;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const short value = decode_i16(&buffer[frame + channel * 2u]);
                minimum[channel] = std::min(minimum[channel], value);
                maximum[channel] = std::max(maximum[channel], value);
            }
        }
        std::string encoded;
        encoded.reserve(channels * 4u);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            append_i16(encoded, minimum[channel]);
            append_i16(encoded, maximum[channel]);
        }
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        if (!output) {
            error = "unable to write viewer pyramid";
            return false;
        }
        remaining -= samples;
    }
    return true;
}

bool build_next_pyramid(
    const std::string& input_path,
    const std::string& output_path,
    std::size_t channel_count,
    unsigned long long input_blocks,
    unsigned long long& output_blocks,
    std::string& error
) {
    std::ifstream input(input_path.c_str(), std::ios::binary);
    std::ofstream output(output_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        error = "unable to open viewer pyramid level";
        return false;
    }
    const std::size_t record_bytes = channel_count * 4u;
    std::vector<char> first(record_bytes);
    std::vector<char> second(record_bytes);
    output_blocks = (input_blocks + 1u) / 2u;
    for (unsigned long long block = 0; block < output_blocks; ++block) {
        input.read(&first[0], static_cast<std::streamsize>(record_bytes));
        if (input.gcount() != static_cast<std::streamsize>(record_bytes)) {
            error = "viewer pyramid level is truncated";
            return false;
        }
        const bool has_second = block * 2u + 1u < input_blocks;
        if (has_second) {
            input.read(&second[0], static_cast<std::streamsize>(record_bytes));
            if (input.gcount() != static_cast<std::streamsize>(record_bytes)) {
                error = "viewer pyramid level is truncated";
                return false;
            }
        }
        std::string encoded;
        encoded.reserve(record_bytes);
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            const short first_min = decode_i16(&first[channel * 4u]);
            const short first_max = decode_i16(&first[channel * 4u + 2u]);
            short minimum = first_min;
            short maximum = first_max;
            if (has_second) {
                minimum = std::min(
                    minimum, decode_i16(&second[channel * 4u]));
                maximum = std::max(
                    maximum, decode_i16(&second[channel * 4u + 2u]));
            }
            append_i16(encoded, minimum);
            append_i16(encoded, maximum);
        }
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        if (!output) {
            error = "unable to write viewer pyramid level";
            return false;
        }
    }
    return true;
}

bool build_pyramid(
    const std::string& case_root,
    const ParsedWfdb& metadata,
    std::string& error
) {
    unsigned long long block_size = kPyramidBaseBlock;
    unsigned long long block_count = 0;
    std::string previous = pyramid_path(case_root, block_size);
    if (!build_base_pyramid(
            case_root + "/signal.dat",
            previous,
            metadata,
            block_count,
            error)) {
        return false;
    }
    while (block_count > 1u &&
           block_size <= std::numeric_limits<unsigned long long>::max() / 2u) {
        const unsigned long long next_size = block_size * 2u;
        const std::string next = pyramid_path(case_root, next_size);
        unsigned long long next_count = 0;
        if (!build_next_pyramid(
                previous,
                next,
                metadata.channels.size(),
                block_count,
                next_count,
                error)) {
            return false;
        }
        block_size = next_size;
        block_count = next_count;
        previous = next;
    }
    return true;
}

bool list_case_names(
    const std::string& cases_root,
    std::vector<std::string>& names,
    std::string& error
) {
    names.clear();
    DIR* handle = opendir(cases_root.c_str());
    if (handle == nullptr) {
        if (errno == ENOENT) return true;
        error = "unable to open viewer cases";
        return false;
    }
    for (dirent* entry = readdir(handle);
         entry != nullptr; entry = readdir(handle)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (!safe_component(name)) {
            closedir(handle);
            error = "viewer case path is unsafe";
            return false;
        }
        if (directory(cases_root + "/" + name)) names.push_back(name);
    }
    closedir(handle);
    std::sort(names.begin(), names.end());
    return true;
}

syn_sig_ra::SignalViewerStatus load_case(
    const std::string& viewer_root,
    const std::string& case_id,
    syn_sig_ra::SignalViewerCase& output,
    std::string& error
) {
    if (!safe_component(case_id)) {
        error = "case_id is invalid";
        return syn_sig_ra::SignalViewerStatus::invalid_request;
    }
    const std::string case_root = viewer_root + "/cases/" + case_id;
    if (!directory(case_root)) return syn_sig_ra::SignalViewerStatus::not_found;
    ParsedWfdb parsed;
    if (!parse_wfdb_header(
            case_root + "/signal.hea",
            case_root + "/signal.dat",
            parsed,
            error)) {
        return syn_sig_ra::SignalViewerStatus::invalid_source;
    }
    if (!regular_file(pyramid_path(case_root, kPyramidBaseBlock))) {
        error = "viewer pyramid is unavailable";
        return syn_sig_ra::SignalViewerStatus::invalid_source;
    }
    syn_sig_ra::SignalViewerCase fresh;
    fresh.case_id = case_id;
    fresh.sample_rate_hz = parsed.sample_rate_hz;
    fresh.sample_count = parsed.sample_count;
    fresh.channels = parsed.channels;
    output = fresh;
    return syn_sig_ra::SignalViewerStatus::ok;
}

bool normalize_channels(
    const syn_sig_ra::SignalViewerWindowRequest& request,
    unsigned int available,
    std::vector<unsigned int>& selected,
    std::string& error
) {
    selected = request.channel_indices;
    if (selected.empty()) {
        for (unsigned int index = 0; index < available; ++index) {
            selected.push_back(index);
        }
    }
    std::vector<bool> seen(available, false);
    for (std::vector<unsigned int>::const_iterator it = selected.begin();
         it != selected.end(); ++it) {
        if (*it >= available || seen[*it]) {
            error = "channel selection is invalid";
            return false;
        }
        seen[*it] = true;
    }
    return !selected.empty();
}

}  // namespace

namespace syn_sig_ra {

SignalViewerStatus prepare_signal_viewer_source(
    const std::string& extracted_package_root,
    const std::string& viewer_root,
    std::string& error
) {
    error.clear();
    if (directory(viewer_root) || mkdir(viewer_root.c_str(), 0750) != 0) {
        error = "viewer output directory already exists or cannot be created";
        return SignalViewerStatus::io_error;
    }
    const std::string viewer_cases = viewer_root + "/cases";
    if (mkdir(viewer_cases.c_str(), 0750) != 0) {
        remove_tree(viewer_root);
        error = "unable to create viewer case directory";
        return SignalViewerStatus::io_error;
    }
    std::vector<std::string> cases;
    if (!list_case_names(extracted_package_root + "/cases", cases, error)) {
        remove_tree(viewer_root);
        return SignalViewerStatus::io_error;
    }
    unsigned int prepared = 0;
    for (std::vector<std::string>::const_iterator it = cases.begin();
         it != cases.end(); ++it) {
        const std::string source_case = extracted_package_root + "/cases/" + *it;
        const std::string source_header = source_case + "/synsigra.hea";
        const std::string source_data = source_case + "/synsigra.dat";
        if (!regular_file(source_header) && !regular_file(source_data)) continue;
        ParsedWfdb metadata;
        if (!parse_wfdb_header(source_header, source_data, metadata, error)) {
            remove_tree(viewer_root);
            return SignalViewerStatus::invalid_source;
        }
        const std::string destination_case = viewer_cases + "/" + *it;
        if (mkdir(destination_case.c_str(), 0750) != 0 ||
            !copy_file(source_header, destination_case + "/signal.hea", error)) {
            remove_tree(viewer_root);
            if (error.empty()) error = "unable to create viewer case";
            return SignalViewerStatus::io_error;
        }
        if (link(source_data.c_str(), (destination_case + "/signal.dat").c_str()) != 0 &&
            !copy_file(source_data, destination_case + "/signal.dat", error)) {
            remove_tree(viewer_root);
            return SignalViewerStatus::io_error;
        }
        if (!build_pyramid(destination_case, metadata, error)) {
            remove_tree(viewer_root);
            return SignalViewerStatus::io_error;
        }
        ++prepared;
    }
    if (prepared == 0u) {
        remove_tree(viewer_root);
        error = "package contains no viewable WFDB cases";
        return SignalViewerStatus::not_found;
    }
    return SignalViewerStatus::ok;
}

SignalViewerStatus describe_signal_viewer_source(
    const std::string& viewer_root,
    SignalViewerSource& source,
    std::string& error
) {
    error.clear();
    if (!directory(viewer_root + "/cases")) {
        error = "viewer source is unavailable";
        return SignalViewerStatus::not_found;
    }
    std::vector<std::string> names;
    if (!list_case_names(viewer_root + "/cases", names, error)) {
        return SignalViewerStatus::io_error;
    }
    SignalViewerSource fresh;
    for (std::vector<std::string>::const_iterator it = names.begin();
         it != names.end(); ++it) {
        SignalViewerCase item;
        const SignalViewerStatus status = load_case(viewer_root, *it, item, error);
        if (status != SignalViewerStatus::ok) return status;
        fresh.cases.push_back(item);
    }
    if (fresh.cases.empty()) {
        error = "viewer source contains no cases";
        return SignalViewerStatus::not_found;
    }
    source = fresh;
    return SignalViewerStatus::ok;
}

SignalViewerStatus read_signal_viewer_window(
    const std::string& viewer_root,
    const SignalViewerWindowRequest& request,
    SignalViewerWindow& window,
    std::string& error
) {
    error.clear();
    if (request.max_points == 0u || request.max_points > kMaximumPoints ||
        request.sample_count == 0u) {
        error = "viewport size or points are invalid";
        return SignalViewerStatus::invalid_request;
    }
    SignalViewerCase metadata;
    const SignalViewerStatus case_status =
        load_case(viewer_root, request.case_id, metadata, error);
    if (case_status != SignalViewerStatus::ok) return case_status;
    if (request.start_sample >= metadata.sample_count) {
        error = "viewport starts outside the signal";
        return SignalViewerStatus::invalid_request;
    }
    const unsigned long long span = std::min(
        request.sample_count,
        metadata.sample_count - request.start_sample
    );
    std::vector<unsigned int> selected;
    if (!normalize_channels(
            request,
            static_cast<unsigned int>(metadata.channels.size()),
            selected,
            error)) {
        return SignalViewerStatus::invalid_request;
    }

    const unsigned long long desired_block =
        1u + (span - 1u) / request.max_points;
    unsigned long long block_size = std::max(1ull, desired_block);
    bool use_pyramid = block_size >= kPyramidBaseBlock;
    if (use_pyramid) {
        block_size = kPyramidBaseBlock;
        while (block_size < desired_block &&
               block_size <= std::numeric_limits<unsigned long long>::max() / 2u) {
            block_size *= 2u;
        }
    }
    const unsigned long long start_block = request.start_sample / block_size;
    const unsigned long long request_end = request.start_sample + span;
    unsigned long long past_block = request_end / block_size;
    if (request_end % block_size != 0u) ++past_block;
    const unsigned long long total_blocks =
        1u + (metadata.sample_count - 1u) / block_size;
    past_block = std::min(past_block, total_blocks);
    const unsigned long long bucket_count_64 = past_block - start_block;
    if (bucket_count_64 == 0u || bucket_count_64 > kMaximumPoints + 2u ||
        bucket_count_64 > std::numeric_limits<unsigned int>::max()) {
        error = "viewport aggregation exceeds the response bound";
        return SignalViewerStatus::invalid_request;
    }
    const unsigned int bucket_count = static_cast<unsigned int>(bucket_count_64);
    const std::size_t selected_count = selected.size();
    std::vector<std::vector<short> > minimum(
        selected_count, std::vector<short>(bucket_count));
    std::vector<std::vector<short> > maximum(
        selected_count, std::vector<short>(bucket_count));
    const std::string case_root = viewer_root + "/cases/" + request.case_id;

    if (use_pyramid) {
        const std::size_t record_bytes = metadata.channels.size() * 4u;
        const unsigned long long byte_offset_64 = start_block * record_bytes;
        const unsigned long long byte_count_64 = bucket_count_64 * record_bytes;
        if (byte_offset_64 > static_cast<unsigned long long>(
                std::numeric_limits<std::streamoff>::max()) ||
            byte_count_64 > static_cast<unsigned long long>(
                std::numeric_limits<std::size_t>::max())) {
            error = "viewer pyramid offset is too large";
            return SignalViewerStatus::io_error;
        }
        std::ifstream input(
            pyramid_path(case_root, block_size).c_str(), std::ios::binary);
        if (!input) {
            error = "viewer pyramid level is unavailable";
            return SignalViewerStatus::invalid_source;
        }
        input.seekg(static_cast<std::streamoff>(byte_offset_64));
        std::vector<char> encoded(static_cast<std::size_t>(byte_count_64));
        input.read(&encoded[0], static_cast<std::streamsize>(encoded.size()));
        if (input.gcount() != static_cast<std::streamsize>(encoded.size())) {
            error = "viewer pyramid level is truncated";
            return SignalViewerStatus::invalid_source;
        }
        for (unsigned int bucket = 0; bucket < bucket_count; ++bucket) {
            const std::size_t record = static_cast<std::size_t>(bucket) * record_bytes;
            for (std::size_t output_channel = 0;
                 output_channel < selected_count; ++output_channel) {
                const std::size_t channel = selected[output_channel];
                minimum[output_channel][bucket] =
                    decode_i16(&encoded[record + channel * 4u]);
                maximum[output_channel][bucket] =
                    decode_i16(&encoded[record + channel * 4u + 2u]);
            }
        }
    } else {
        const std::size_t frame_bytes = metadata.channels.size() * 2u;
        const unsigned long long actual_start = start_block * block_size;
        const unsigned long long byte_offset_64 = actual_start * frame_bytes;
        if (byte_offset_64 > static_cast<unsigned long long>(
                std::numeric_limits<std::streamoff>::max())) {
            error = "WFDB viewport offset is too large";
            return SignalViewerStatus::io_error;
        }
        std::ifstream input((case_root + "/signal.dat").c_str(), std::ios::binary);
        if (!input) {
            error = "viewer signal file is unavailable";
            return SignalViewerStatus::invalid_source;
        }
        input.seekg(static_cast<std::streamoff>(byte_offset_64));
        std::vector<char> encoded(static_cast<std::size_t>(block_size) * frame_bytes);
        for (unsigned int bucket = 0; bucket < bucket_count; ++bucket) {
            const unsigned long long bucket_start =
                (start_block + bucket) * block_size;
            const unsigned long long samples = std::min(
                block_size, metadata.sample_count - bucket_start);
            const std::size_t bytes = static_cast<std::size_t>(samples) * frame_bytes;
            input.read(&encoded[0], static_cast<std::streamsize>(bytes));
            if (input.gcount() != static_cast<std::streamsize>(bytes)) {
                error = "viewer signal file is truncated";
                return SignalViewerStatus::invalid_source;
            }
            for (std::size_t output_channel = 0;
                 output_channel < selected_count; ++output_channel) {
                const std::size_t channel = selected[output_channel];
                short low = decode_i16(&encoded[channel * 2u]);
                short high = low;
                for (unsigned long long sample = 1; sample < samples; ++sample) {
                    const short value = decode_i16(
                        &encoded[static_cast<std::size_t>(sample) * frame_bytes +
                                 channel * 2u]);
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
                minimum[output_channel][bucket] = low;
                maximum[output_channel][bucket] = high;
            }
        }
    }

    SignalViewerWindow fresh;
    fresh.requested_start_sample = request.start_sample;
    fresh.requested_sample_count = span;
    fresh.data_start_sample = start_block * block_size;
    fresh.samples_per_bucket = block_size;
    fresh.source_sample_count = metadata.sample_count;
    fresh.sample_rate_hz = metadata.sample_rate_hz;
    fresh.bucket_count = bucket_count;
    fresh.channel_indices = selected;
    fresh.binary.reserve(
        80u + selected_count * 4u + selected_count * bucket_count * 4u);
    fresh.binary.append("SYNSIGV1", 8u);
    append_u16(fresh.binary, 1u);
    append_u16(fresh.binary, 80u);
    append_u32(fresh.binary, 1u);  // channel-major signed-int16 min/max
    append_u32(fresh.binary, static_cast<unsigned int>(selected_count));
    append_u32(fresh.binary, bucket_count);
    append_u64(fresh.binary, fresh.requested_start_sample);
    append_u64(fresh.binary, fresh.requested_sample_count);
    append_u64(fresh.binary, fresh.data_start_sample);
    append_u64(fresh.binary, fresh.samples_per_bucket);
    append_u64(fresh.binary, fresh.source_sample_count);
    append_f64(fresh.binary, fresh.sample_rate_hz);
    append_u32(fresh.binary, 80u + static_cast<unsigned int>(selected_count) * 4u);
    append_u32(fresh.binary, 0u);
    for (std::vector<unsigned int>::const_iterator it = selected.begin();
         it != selected.end(); ++it) {
        append_u32(fresh.binary, *it);
    }
    for (std::size_t channel = 0; channel < selected_count; ++channel) {
        for (unsigned int bucket = 0; bucket < bucket_count; ++bucket) {
            append_i16(fresh.binary, minimum[channel][bucket]);
            append_i16(fresh.binary, maximum[channel][bucket]);
        }
    }
    window = fresh;
    return SignalViewerStatus::ok;
}

const char* signal_viewer_binary_content_type() {
    return "application/vnd.synsigra.signal-window.v1";
}

}  // namespace syn_sig_ra
