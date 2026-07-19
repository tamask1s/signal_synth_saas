#ifndef SYN_SIG_RA_ARTIFACT_STORE_H
#define SYN_SIG_RA_ARTIFACT_STORE_H

#include <string>
#include <vector>

namespace syn_sig_ra {

struct StoredPackage {
    std::string package_id;
    std::string package_directory;
    std::string manifest_hash;
    long long size_bytes;
};

struct PreparedArtifact {
    std::string path;
    std::string sha256;
    long long size_bytes = 0;
    bool created = false;
};

enum class DerivedArtifactStatus {
    ok,
    invalid_source,
    storage_pressure,
    io_error
};

enum class ByteRangeStatus {
    none,
    valid,
    invalid
};

struct ByteRange {
    long long offset = 0;
    long long length = 0;
};

bool store_immutable_package(
    const std::string& data_root,
    const std::string& rendered_directory,
    StoredPackage& package,
    std::string& error
);

bool discard_stored_package(
    const std::string& data_root,
    const std::string& package_id,
    std::string& error
);

bool inspect_challenge_package(
    const std::string& helper_path,
    const std::string& verifier_wheel,
    const std::string& challenge_path,
    std::string& metadata_json,
    std::string& error
);

DerivedArtifactStatus prepare_verification_kit(
    const std::string& data_root,
    const std::string& package_id,
    const std::string& source_zip,
    const std::string& helper_path,
    const std::string& verifier_wheel,
    PreparedArtifact& artifact,
    std::string& metadata_json,
    std::string& error
);

bool purge_account_storage(
    const std::string& data_root,
    const std::vector<std::string>& package_ids,
    const std::vector<std::string>& job_ids,
    const std::vector<std::string>& custom_pack_ids,
    std::string& error
);

DerivedArtifactStatus prepare_cached_file_metadata(
    const std::string& data_root,
    const std::string& package_id,
    const std::string& source_path,
    const std::string& cache_key,
    PreparedArtifact& artifact,
    std::string& error
);

ByteRangeStatus parse_byte_range(
    const std::string& header,
    long long file_size,
    ByteRange& range
);

}  // namespace syn_sig_ra

#endif
