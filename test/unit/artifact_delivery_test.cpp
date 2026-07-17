#include "syn_sig_ra/artifact_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    const std::string root =
        "/tmp/syn_sig_ra_artifact_delivery_" + std::to_string(getpid());
    require(mkdir(root.c_str(), 0700) == 0, "fixture root should be created");
    const std::string source = root + "/source.zip";
    {
        std::ofstream output(source.c_str(), std::ios::binary);
        output << "bounded immutable artifact";
    }
    syn_sig_ra::PreparedArtifact first;
    std::string error;
    require(
        syn_sig_ra::prepare_cached_file_metadata(
            root, "pkg_delivery_fixture", source, "package-zip", first, error) ==
            syn_sig_ra::DerivedArtifactStatus::ok,
        "streaming checksum metadata should be prepared: " + error
    );
    require(
        first.path == source && first.size_bytes == 26 &&
            first.sha256.size() == 64,
        "prepared metadata should identify the immutable file"
    );
    syn_sig_ra::PreparedArtifact second;
    require(
        syn_sig_ra::prepare_cached_file_metadata(
            root, "pkg_delivery_fixture", source, "package-zip", second, error) ==
            syn_sig_ra::DerivedArtifactStatus::ok &&
            second.sha256 == first.sha256,
        "checksum sidecar should be stable and reusable"
    );

    syn_sig_ra::ByteRange range;
    require(
        syn_sig_ra::parse_byte_range("", 1000, range) ==
            syn_sig_ra::ByteRangeStatus::none,
        "an absent range should select the complete file"
    );
    require(
        syn_sig_ra::parse_byte_range("bytes=100-199", 1000, range) ==
            syn_sig_ra::ByteRangeStatus::valid &&
            range.offset == 100 && range.length == 100,
        "a bounded byte range should be parsed exactly"
    );
    require(
        syn_sig_ra::parse_byte_range("bytes=900-", 1000, range) ==
            syn_sig_ra::ByteRangeStatus::valid &&
            range.offset == 900 && range.length == 100,
        "an open-ended byte range should reach EOF"
    );
    require(
        syn_sig_ra::parse_byte_range("bytes=-64", 1000, range) ==
            syn_sig_ra::ByteRangeStatus::valid &&
            range.offset == 936 && range.length == 64,
        "a suffix byte range should select the file tail"
    );
    require(
        syn_sig_ra::parse_byte_range("bytes=1000-", 1000, range) ==
            syn_sig_ra::ByteRangeStatus::invalid &&
        syn_sig_ra::parse_byte_range("bytes=0-1,4-5", 1000, range) ==
            syn_sig_ra::ByteRangeStatus::invalid,
        "unsatisfiable and multipart ranges should fail safely"
    );

    const std::string sparse = root + "/sparse-large.zip";
    const int descriptor = open(sparse.c_str(), O_CREAT | O_RDWR, 0600);
    const long long sparse_size = 5ll * 1024ll * 1024ll * 1024ll + 123ll;
    require(
        descriptor >= 0 && ftruncate(descriptor, sparse_size) == 0,
        "a sparse file larger than 4 GiB should be representable"
    );
    if (descriptor >= 0) close(descriptor);
    require(
        syn_sig_ra::parse_byte_range(
            "bytes=4294967296-4294967311", sparse_size, range) ==
            syn_sig_ra::ByteRangeStatus::valid &&
            range.offset == 4294967296ll && range.length == 16,
        "range offsets must remain 64-bit beyond 4 GiB"
    );

    const std::string package_root = root + "/packages";
    const std::string package = package_root + "/pkg_account_fixture";
    const std::string extracted = package + "/extracted";
    const std::string derived =
        root + "/derived-artifacts/pkg_account_fixture";
    const std::string recipes_root = root + "/recipes";
    const std::string recipe = recipes_root + "/job_account_fixture";
    const std::string work_root = root + "/work";
    const std::string work = work_root + "/job_account_fixture";
    const std::string custom_root = root + "/custom_packs";
    const std::string custom = custom_root + "/custom_pack_account_fixture";
    require(
        mkdir(package_root.c_str(), 0700) == 0 &&
            mkdir(package.c_str(), 0700) == 0 &&
            mkdir(extracted.c_str(), 0700) == 0 &&
            mkdir(derived.c_str(), 0700) == 0 &&
            mkdir(recipes_root.c_str(), 0700) == 0 &&
            mkdir(recipe.c_str(), 0700) == 0 &&
            mkdir(work_root.c_str(), 0700) == 0 &&
            mkdir(work.c_str(), 0700) == 0 &&
            mkdir(custom_root.c_str(), 0700) == 0 &&
            mkdir(custom.c_str(), 0700) == 0,
        "account-owned artifact fixtures should be created"
    );
    {
        std::ofstream output((extracted + "/signal.dat").c_str());
        output << "immutable";
    }
    {
        std::ofstream output((derived + "/package.sha256").c_str());
        output << "checksum";
    }
    chmod((extracted + "/signal.dat").c_str(), 0440);
    chmod(extracted.c_str(), 0550);
    chmod(package.c_str(), 0550);
    require(
        syn_sig_ra::purge_account_storage(
            root,
            std::vector<std::string>(1, "pkg_account_fixture"),
            std::vector<std::string>(1, "job_account_fixture"),
            std::vector<std::string>(1, "custom_pack_account_fixture"),
            error),
        "account deletion should remove immutable and derived server artifacts: " +
            error
    );
    require(
        access(package.c_str(), F_OK) != 0 &&
            access(derived.c_str(), F_OK) != 0 &&
            access(recipe.c_str(), F_OK) != 0 &&
            access(work.c_str(), F_OK) != 0 &&
            access(custom.c_str(), F_OK) != 0,
        "all server-side account artifact families should be removed"
    );
    require(
        !syn_sig_ra::purge_account_storage(
            root, std::vector<std::string>(1, "../escape"),
            std::vector<std::string>(), std::vector<std::string>(), error),
        "account cleanup should reject path traversal identities"
    );

    unlink(sparse.c_str());
    unlink(source.c_str());
    const std::string cache = root + "/derived-artifacts/pkg_delivery_fixture";
    unlink((cache + "/package-zip.sha256").c_str());
    unlink((cache + "/build.lock").c_str());
    rmdir(cache.c_str());
    rmdir((root + "/derived-artifacts").c_str());
    rmdir(package_root.c_str());
    rmdir(recipes_root.c_str());
    rmdir(work_root.c_str());
    rmdir(custom_root.c_str());
    rmdir(root.c_str());
    std::cout << "artifact delivery tests passed\n";
    return EXIT_SUCCESS;
}
