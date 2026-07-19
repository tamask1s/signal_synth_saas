#include "syn_sig_ra/signal_viewer.h"

#include <jansson.h>
#include <sqlite3.h>

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
const unsigned int kMaximumOverlayItems = 10000u;

struct ParsedWfdb {
    std::string data_file;
    double sample_rate_hz;
    unsigned long long sample_count;
    std::vector<syn_sig_ra::SignalViewerChannel> channels;
};

bool regular_file(const std::string& path, long long* size);

const char* json_text(json_t* object, const char* key, const char* fallback = "") {
    json_t* value = json_is_object(object) ? json_object_get(object, key) : nullptr;
    return json_is_string(value) ? json_string_value(value) : fallback;
}

bool json_bool(json_t* object, const char* key, bool fallback = false) {
    json_t* value = json_is_object(object) ? json_object_get(object, key) : nullptr;
    return json_is_boolean(value) ? json_is_true(value) : fallback;
}

bool json_number(json_t* object, const char* key, double& output) {
    json_t* value = json_is_object(object) ? json_object_get(object, key) : nullptr;
    if (!json_is_number(value)) return false;
    output = json_number_value(value);
    return std::isfinite(output);
}

bool json_index(json_t* object, const char* key, unsigned long long& output) {
    json_t* value = json_is_object(object) ? json_object_get(object, key) : nullptr;
    if (json_is_integer(value) && json_integer_value(value) >= 0) {
        output = static_cast<unsigned long long>(json_integer_value(value));
        return true;
    }
    if (!json_is_real(value)) return false;
    const double number = json_real_value(value);
    if (!std::isfinite(number) || number < 0.0 ||
        number > 9007199254740991.0 || std::floor(number) != number) return false;
    output = static_cast<unsigned long long>(number);
    return true;
}

bool json_signed_index(json_t* object, const char* key, long long& output) {
    json_t* value = json_is_object(object) ? json_object_get(object, key) : nullptr;
    if (json_is_integer(value)) {
        output = static_cast<long long>(json_integer_value(value));
        return true;
    }
    if (!json_is_real(value)) return false;
    const double number = json_real_value(value);
    if (!std::isfinite(number) || number < -1.0 ||
        number > 9007199254740991.0 || std::floor(number) != number) return false;
    output = static_cast<long long>(number);
    return true;
}

unsigned long long sample_at_seconds(
    double seconds,
    const ParsedWfdb& metadata,
    bool allow_past_end = false
) {
    if (!std::isfinite(seconds) || seconds <= 0.0) return 0u;
    const long double scaled = static_cast<long double>(seconds) *
        static_cast<long double>(metadata.sample_rate_hz);
    unsigned long long sample = scaled >= static_cast<long double>(
        std::numeric_limits<unsigned long long>::max())
        ? std::numeric_limits<unsigned long long>::max()
        : static_cast<unsigned long long>(std::llround(scaled));
    const unsigned long long maximum = allow_past_end
        ? metadata.sample_count
        : metadata.sample_count - 1u;
    return std::min(sample, maximum);
}

std::string json_string_list(json_t* value) {
    if (!json_is_array(value)) return "";
    std::ostringstream joined;
    for (std::size_t index = 0; index < json_array_size(value); ++index) {
        json_t* item = json_array_get(value, index);
        if (!json_is_string(item)) continue;
        if (joined.tellp() > 0) joined << ',';
        joined << json_string_value(item);
    }
    return joined.str();
}

bool execute_sql(sqlite3* database, const char* sql, std::string& error) {
    char* message = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &message) == SQLITE_OK) {
        return true;
    }
    error = message == nullptr ? sqlite3_errmsg(database) : message;
    sqlite3_free(message);
    return false;
}

bool insert_overlay(
    sqlite3_stmt* statement,
    const char* kind,
    const char* source,
    const std::string& label,
    const std::string& channel,
    unsigned long long start_sample,
    unsigned long long end_sample,
    bool interval,
    bool has_value,
    double value,
    std::string& error
) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    const bool bound =
        sqlite3_bind_text(statement, 1, kind, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, source, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 3, label.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 4, channel.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(statement, 5, static_cast<sqlite3_int64>(start_sample)) == SQLITE_OK &&
        sqlite3_bind_int64(statement, 6, static_cast<sqlite3_int64>(end_sample)) == SQLITE_OK &&
        sqlite3_bind_int(statement, 7, interval ? 1 : 0) == SQLITE_OK &&
        (has_value
            ? sqlite3_bind_double(statement, 8, value) == SQLITE_OK
            : sqlite3_bind_null(statement, 8) == SQLITE_OK);
    if (bound && sqlite3_step(statement) == SQLITE_DONE) return true;
    error = sqlite3_errmsg(sqlite3_db_handle(statement));
    return false;
}

bool build_overlay_database(
    const std::string& annotations_path,
    const std::string& database_path,
    const ParsedWfdb& metadata,
    std::string& error
) {
    sqlite3* database = nullptr;
    sqlite3_stmt* insert = nullptr;
    if (sqlite3_open_v2(
            database_path.c_str(), &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
            nullptr) != SQLITE_OK) {
        error = database == nullptr ? "unable to create overlay index" :
            sqlite3_errmsg(database);
        if (database != nullptr) sqlite3_close(database);
        return false;
    }
    const char* schema =
        "PRAGMA journal_mode=OFF;"
        "PRAGMA synchronous=OFF;"
        "PRAGMA temp_store=MEMORY;"
        "CREATE TABLE overlays("
        "id INTEGER PRIMARY KEY,kind TEXT NOT NULL,source TEXT NOT NULL,"
        "label TEXT NOT NULL,channel TEXT NOT NULL,start_sample INTEGER NOT NULL,"
        "end_sample INTEGER NOT NULL,is_interval INTEGER NOT NULL,value REAL);"
        "CREATE INDEX overlays_point_range ON overlays(is_interval,start_sample);"
        "CREATE INDEX overlays_interval_range ON overlays(is_interval,start_sample,end_sample);"
        "PRAGMA user_version=1;"
        "BEGIN IMMEDIATE;";
    const char* insert_sql =
        "INSERT INTO overlays(kind,source,label,channel,start_sample,end_sample,is_interval,value) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);";
    bool succeeded = execute_sql(database, schema, error) &&
        sqlite3_prepare_v2(database, insert_sql, -1, &insert, nullptr) == SQLITE_OK;
    if (!succeeded && error.empty()) error = sqlite3_errmsg(database);

    json_t* root = nullptr;
    if (succeeded && regular_file(annotations_path, nullptr)) {
        json_error_t json_error;
        root = json_load_file(
            annotations_path.c_str(),
            JSON_REJECT_DUPLICATES | JSON_DECODE_INT_AS_REAL,
            &json_error);
        if (!json_is_object(root)) {
            std::ostringstream message;
            message << "annotations.json is invalid at line " << json_error.line
                    << ", column " << json_error.column << ": "
                    << json_error.text;
            error = message.str();
            succeeded = false;
        }
    }

    if (succeeded && root != nullptr) {
        json_t* beats = json_object_get(root, "beats");
        for (std::size_t index = 0;
             succeeded && json_is_array(beats) && index < json_array_size(beats);
             ++index) {
            json_t* beat = json_array_get(beats, index);
            double seconds = 0.0;
            if (!json_number(beat, "r_peak_seconds", seconds)) continue;
            const unsigned long long sample = sample_at_seconds(seconds, metadata);
            succeeded = insert_overlay(
                insert, "r_peak", "ground_truth", "R peak", "ecg",
                sample, sample, false, false, 0.0, error);
            if (succeeded) {
                succeeded = insert_overlay(
                    insert, "beat_class", "ground_truth",
                    json_text(beat, "beat_class", "unknown"), "ecg",
                    sample, sample, false, false, 0.0, error);
            }
        }

        json_t* fiducials = json_object_get(root, "fiducials");
        for (std::size_t index = 0;
             succeeded && json_is_array(fiducials) &&
                 index < json_array_size(fiducials);
             ++index) {
            json_t* fiducial = json_array_get(fiducials, index);
            if (!json_bool(fiducial, "present")) continue;
            const std::string kind(json_text(fiducial, "kind"));
            const std::string source(json_text(fiducial, "source"));
            if (kind.empty() || source.empty()) continue;
            unsigned long long sample = 0;
            double seconds = 0.0;
            if (!json_index(fiducial, "sample_index", sample)) {
                if (!json_number(fiducial, "time_seconds", seconds)) continue;
                sample = sample_at_seconds(seconds, metadata);
            }
            sample = std::min(sample, metadata.sample_count - 1u);
            long long lead_index = -1;
            if (!json_signed_index(fiducial, "lead_index", lead_index) ||
                lead_index < -1) continue;
            std::string channel("ecg");
            if (lead_index >= 0 &&
                static_cast<std::size_t>(lead_index) < metadata.channels.size()) {
                channel = metadata.channels[static_cast<std::size_t>(lead_index)].name;
            }
            double amplitude = 0.0;
            const bool has_amplitude =
                json_number(fiducial, "amplitude_mv", amplitude);
            succeeded = insert_overlay(
                insert, "ecg_fiducial", source.c_str(), kind, channel,
                sample, sample, false, has_amplitude, amplitude, error);
        }

        json_t* ppg = json_object_get(root, "ppg_fiducials");
        for (std::size_t index = 0;
             succeeded && json_is_array(ppg) && index < json_array_size(ppg);
             ++index) {
            json_t* fiducial = json_array_get(ppg, index);
            const std::string kind(json_text(fiducial, "kind"));
            const char* overlay_kind = kind == "pulse_onset" ? "ppg_onset" :
                kind == "systolic_peak" ? "ppg_peak" : nullptr;
            double seconds = 0.0;
            if (overlay_kind == nullptr ||
                !json_number(fiducial, "time_seconds", seconds)) continue;
            unsigned long long sample = 0;
            if (!json_index(fiducial, "sample_index", sample)) {
                sample = sample_at_seconds(seconds, metadata);
            }
            sample = std::min(sample, metadata.sample_count - 1u);
            succeeded = insert_overlay(
                insert, overlay_kind, json_text(fiducial, "source", "ground_truth"),
                kind, "ppg_green", sample, sample, false, false, 0.0, error);
        }

        json_t* episodes = json_object_get(root, "episodes");
        for (std::size_t index = 0;
             succeeded && json_is_array(episodes) && index < json_array_size(episodes);
             ++index) {
            json_t* episode = json_array_get(episodes, index);
            if (!json_bool(episode, "present", true)) continue;
            unsigned long long start = 0, end = 0;
            double start_seconds = 0.0, end_seconds = 0.0;
            if (!json_index(episode, "start_sample_index", start) &&
                json_number(episode, "start_seconds", start_seconds)) {
                start = sample_at_seconds(start_seconds, metadata, true);
            }
            if (!json_index(episode, "end_sample_index", end) &&
                json_number(episode, "end_seconds", end_seconds)) {
                end = sample_at_seconds(end_seconds, metadata, true);
            }
            start = std::min(start, metadata.sample_count);
            end = std::min(end, metadata.sample_count);
            if (end <= start) continue;
            succeeded = insert_overlay(
                insert, "episode", "ground_truth", json_text(episode, "kind", "episode"),
                "ecg", start, end, true, false, 0.0, error);
        }

        json_t* artifacts = json_object_get(root, "artifact_intervals");
        for (std::size_t index = 0;
             succeeded && json_is_array(artifacts) && index < json_array_size(artifacts);
             ++index) {
            json_t* artifact = json_array_get(artifacts, index);
            unsigned long long start = 0, end = 0;
            double start_seconds = 0.0, end_seconds = 0.0;
            if (!json_index(artifact, "start_sample_index", start) &&
                json_number(artifact, "start_seconds", start_seconds)) {
                start = sample_at_seconds(start_seconds, metadata, true);
            }
            if (!json_index(artifact, "end_sample_index", end) &&
                json_number(artifact, "end_seconds", end_seconds)) {
                end = sample_at_seconds(end_seconds, metadata, true);
            }
            start = std::min(start, metadata.sample_count);
            end = std::min(end, metadata.sample_count);
            if (end <= start) continue;
            double severity = 0.0;
            const bool has_severity = json_number(artifact, "severity", severity);
            succeeded = insert_overlay(
                insert, "artifact", "ground_truth", json_text(artifact, "type", "artifact"),
                json_string_list(json_object_get(artifact, "channels")),
                start, end, true, has_severity, severity, error);
        }

        json_t* pulses = json_object_get(root, "ppg_pulses");
        for (std::size_t index = 0;
             succeeded && json_is_array(pulses) && index < json_array_size(pulses);
             ++index) {
            json_t* pulse = json_array_get(pulses, index);
            double peak_seconds = 0.0;
            if (json_bool(pulse, "low_perfusion") &&
                json_number(pulse, "expected_peak_time_seconds", peak_seconds)) {
                const unsigned long long sample = sample_at_seconds(peak_seconds, metadata);
                succeeded = insert_overlay(
                    insert, "low_perfusion", "ground_truth", "Low perfusion pulse",
                    "ppg_green", sample, sample, false, false, 0.0, error);
            }
            double onset_seconds = 0.0, offset_seconds = 0.0;
            if (succeeded && json_bool(pulse, "intentionally_missing") &&
                json_number(pulse, "expected_onset_time_seconds", onset_seconds) &&
                json_number(pulse, "expected_offset_time_seconds", offset_seconds)) {
                const unsigned long long start = sample_at_seconds(onset_seconds, metadata, true);
                const unsigned long long end = sample_at_seconds(offset_seconds, metadata, true);
                if (end > start) {
                    succeeded = insert_overlay(
                        insert, "missing_pulse", "ground_truth", "Expected missing pulse",
                        "ppg_green", start, end, true, false, 0.0, error);
                }
            }
        }
    }

    if (root != nullptr) json_decref(root);
    sqlite3_finalize(insert);
    if (succeeded) succeeded = execute_sql(database, "COMMIT;PRAGMA optimize;", error);
    else execute_sql(database, "ROLLBACK;", error);
    if (sqlite3_close(database) != SQLITE_OK && succeeded) {
        error = "unable to finalize overlay index";
        succeeded = false;
    }
    if (!succeeded) unlink(database_path.c_str());
    return succeeded;
}

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
        if (!build_overlay_database(
                source_case + "/annotations.json",
                destination_case + "/overlays.sqlite3",
                metadata,
                error)) {
            error = "case " + *it + ": " + error;
            remove_tree(viewer_root);
            return SignalViewerStatus::invalid_source;
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

namespace {

std::string overlay_column_text(sqlite3_stmt* statement, int column) {
    const unsigned char* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string() :
        std::string(reinterpret_cast<const char*>(value));
}

SignalViewerOverlayItem overlay_row(
    sqlite3_stmt* statement,
    bool interval,
    unsigned int count = 1u
) {
    SignalViewerOverlayItem item;
    item.kind = overlay_column_text(statement, 0);
    item.source = overlay_column_text(statement, 1);
    item.label = overlay_column_text(statement, 2);
    item.channel = overlay_column_text(statement, 3);
    item.start_sample = static_cast<unsigned long long>(
        std::max<sqlite3_int64>(0, sqlite3_column_int64(statement, 4)));
    item.end_sample = static_cast<unsigned long long>(
        std::max<sqlite3_int64>(0, sqlite3_column_int64(statement, 5)));
    item.interval = interval;
    item.has_value = sqlite3_column_type(statement, 6) != SQLITE_NULL;
    item.value = item.has_value ? sqlite3_column_double(statement, 6) : 0.0;
    item.count = count;
    return item;
}

bool bind_overlay_range(
    sqlite3_stmt* statement,
    unsigned long long start,
    unsigned long long end
) {
    return sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(start)) == SQLITE_OK &&
        sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(end)) == SQLITE_OK;
}

bool query_exact_overlays(
    sqlite3* database,
    unsigned long long start,
    unsigned long long end,
    unsigned int maximum,
    std::vector<SignalViewerOverlayItem>& output,
    std::string& error
) {
    const char* sql =
        "SELECT kind,source,label,channel,start_sample,end_sample,value,is_interval "
        "FROM overlays WHERE "
        "(is_interval=0 AND start_sample>=?1 AND start_sample<?2) OR "
        "(is_interval=1 AND start_sample<?2 AND end_sample>?1) "
        "ORDER BY start_sample,id LIMIT ?3;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_overlay_range(statement, start, end) ||
        sqlite3_bind_int(statement, 3, static_cast<int>(maximum)) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        return false;
    }
    for (;;) {
        const int status = sqlite3_step(statement);
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = sqlite3_errmsg(database);
            sqlite3_finalize(statement);
            return false;
        }
        output.push_back(overlay_row(
            statement, sqlite3_column_int(statement, 7) != 0));
    }
    sqlite3_finalize(statement);
    return true;
}

bool query_intervals(
    sqlite3* database,
    unsigned long long start,
    unsigned long long end,
    unsigned int maximum,
    std::vector<SignalViewerOverlayItem>& output,
    std::string& error
) {
    const char* sql =
        "SELECT kind,source,label,channel,start_sample,end_sample,value "
        "FROM overlays WHERE is_interval=1 AND start_sample<?2 AND end_sample>?1 "
        "ORDER BY start_sample,id LIMIT ?3;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_overlay_range(statement, start, end) ||
        sqlite3_bind_int(statement, 3, static_cast<int>(maximum)) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        return false;
    }
    for (;;) {
        const int status = sqlite3_step(statement);
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = sqlite3_errmsg(database);
            sqlite3_finalize(statement);
            return false;
        }
        output.push_back(overlay_row(statement, true));
    }
    sqlite3_finalize(statement);
    return true;
}

bool query_clustered_points(
    sqlite3* database,
    unsigned long long start,
    unsigned long long end,
    unsigned long long bucket_size,
    unsigned int maximum,
    std::vector<SignalViewerOverlayItem>& output,
    bool& overflow,
    std::string& error
) {
    const char* sql =
        "SELECT kind,source,label,channel,MIN(start_sample),MAX(start_sample),AVG(value),COUNT(*) "
        "FROM overlays WHERE is_interval=0 AND start_sample>=?1 AND start_sample<?2 "
        "GROUP BY kind,source,label,channel,CAST((start_sample-?1)/?3 AS INTEGER) "
        "ORDER BY MIN(start_sample),kind,label LIMIT ?4;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_overlay_range(statement, start, end) ||
        sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(bucket_size)) != SQLITE_OK ||
        sqlite3_bind_int(statement, 4, static_cast<int>(maximum + 1u)) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        return false;
    }
    std::vector<SignalViewerOverlayItem> fresh;
    for (;;) {
        const int status = sqlite3_step(statement);
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = sqlite3_errmsg(database);
            sqlite3_finalize(statement);
            return false;
        }
        const sqlite3_int64 count = sqlite3_column_int64(statement, 7);
        fresh.push_back(overlay_row(
            statement, false,
            static_cast<unsigned int>(std::min<sqlite3_int64>(
                count, std::numeric_limits<unsigned int>::max()))));
    }
    sqlite3_finalize(statement);
    overflow = fresh.size() > maximum;
    if (overflow) fresh.resize(maximum);
    output.insert(output.end(), fresh.begin(), fresh.end());
    return true;
}

}  // namespace

SignalViewerStatus read_signal_viewer_overlays(
    const std::string& viewer_root,
    const SignalViewerOverlayRequest& request,
    SignalViewerOverlayWindow& window,
    std::string& error
) {
    error.clear();
    if (request.sample_count == 0u || request.max_items == 0u ||
        request.max_items > kMaximumOverlayItems) {
        error = "overlay viewport size or item bound is invalid";
        return SignalViewerStatus::invalid_request;
    }
    SignalViewerCase metadata;
    const SignalViewerStatus case_status =
        load_case(viewer_root, request.case_id, metadata, error);
    if (case_status != SignalViewerStatus::ok) return case_status;
    if (request.start_sample >= metadata.sample_count) {
        error = "overlay viewport starts outside the signal";
        return SignalViewerStatus::invalid_request;
    }
    const unsigned long long span = std::min(
        request.sample_count, metadata.sample_count - request.start_sample);
    const unsigned long long end = request.start_sample + span;
    const std::string database_path = viewer_root + "/cases/" +
        request.case_id + "/overlays.sqlite3";
    if (!regular_file(database_path)) {
        error = "viewer overlay index is unavailable";
        return SignalViewerStatus::not_found;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(
            database_path.c_str(), &database,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        error = database == nullptr ? "unable to open overlay index" :
            sqlite3_errmsg(database);
        if (database != nullptr) sqlite3_close(database);
        return SignalViewerStatus::io_error;
    }
    sqlite3_busy_timeout(database, 1000);

    SignalViewerOverlayWindow fresh;
    fresh.requested_start_sample = request.start_sample;
    fresh.requested_sample_count = span;
    fresh.source_sample_count = metadata.sample_count;
    fresh.sample_rate_hz = metadata.sample_rate_hz;
    fresh.total_matching_items = 0u;
    fresh.aggregated = false;

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "SELECT DISTINCT kind FROM overlays ORDER BY kind;",
            -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        sqlite3_close(database);
        return SignalViewerStatus::invalid_source;
    }
    for (;;) {
        const int status = sqlite3_step(statement);
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            error = sqlite3_errmsg(database);
            sqlite3_finalize(statement);
            sqlite3_close(database);
            return SignalViewerStatus::invalid_source;
        }
        fresh.available_kinds.push_back(overlay_column_text(statement, 0));
    }
    sqlite3_finalize(statement);

    const char* count_sql =
        "SELECT COUNT(*) FROM overlays WHERE "
        "(is_interval=0 AND start_sample>=?1 AND start_sample<?2) OR "
        "(is_interval=1 AND start_sample<?2 AND end_sample>?1);";
    if (sqlite3_prepare_v2(database, count_sql, -1, &statement, nullptr) != SQLITE_OK ||
        !bind_overlay_range(statement, request.start_sample, end) ||
        sqlite3_step(statement) != SQLITE_ROW) {
        error = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return SignalViewerStatus::invalid_source;
    }
    fresh.total_matching_items = static_cast<unsigned long long>(
        std::max<sqlite3_int64>(0, sqlite3_column_int64(statement, 0)));
    sqlite3_finalize(statement);

    bool succeeded = true;
    if (fresh.total_matching_items <= request.max_items) {
        succeeded = query_exact_overlays(
            database, request.start_sample, end, request.max_items,
            fresh.items, error);
    } else {
        fresh.aggregated = true;
        succeeded = query_intervals(
            database, request.start_sample, end, request.max_items,
            fresh.items, error);
        const unsigned int remaining = fresh.items.size() >= request.max_items
            ? 0u
            : request.max_items - static_cast<unsigned int>(fresh.items.size());
        if (succeeded && remaining > 0u) {
            unsigned long long bucket_size = std::max<unsigned long long>(
                1u, (span + std::max(remaining, 1u) - 1u) /
                    std::max(remaining, 1u));
            bool overflow = true;
            while (succeeded && overflow) {
                std::vector<SignalViewerOverlayItem> points;
                succeeded = query_clustered_points(
                    database, request.start_sample, end, bucket_size,
                    remaining, points, overflow, error);
                if (!overflow) {
                    fresh.items.insert(
                        fresh.items.end(), points.begin(), points.end());
                    break;
                }
                if (bucket_size >= span) {
                    // A pathological number of distinct labels is still kept
                    // bounded deterministically by the SQL limit.
                    fresh.items.insert(
                        fresh.items.end(), points.begin(), points.end());
                    overflow = false;
                    break;
                }
                bucket_size = std::min(span, bucket_size * 2u);
            }
        }
        std::sort(
            fresh.items.begin(), fresh.items.end(),
            [](const SignalViewerOverlayItem& left,
               const SignalViewerOverlayItem& right) {
                if (left.start_sample != right.start_sample) {
                    return left.start_sample < right.start_sample;
                }
                return left.kind < right.kind;
            });
    }
    sqlite3_close(database);
    if (!succeeded) return SignalViewerStatus::invalid_source;
    window = fresh;
    return SignalViewerStatus::ok;
}

const char* signal_viewer_binary_content_type() {
    return "application/vnd.synsigra.signal-window.v1";
}

}  // namespace syn_sig_ra
