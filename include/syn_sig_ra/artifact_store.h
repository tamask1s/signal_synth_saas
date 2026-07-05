#ifndef SYN_SIG_RA_ARTIFACT_STORE_H
#define SYN_SIG_RA_ARTIFACT_STORE_H

#include <string>

namespace syn_sig_ra {

struct StoredPackage {
    std::string package_id;
    std::string package_directory;
    std::string manifest_hash;
    long long size_bytes;
};

bool store_immutable_package(
    const std::string& data_root,
    const std::string& rendered_directory,
    StoredPackage& package,
    std::string& error
);

}  // namespace syn_sig_ra

#endif
