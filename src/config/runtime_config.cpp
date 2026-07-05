#include "syn_sig_ra/runtime_config.h"

#include "syn_sig_ra/build_info.h"

#include <sys/stat.h>
#include <unistd.h>

#include <string>

namespace {

bool require_absolute_path(
    const std::string& name,
    const std::string& value,
    std::string& error
) {
    if (value.empty() || value[0] != '/') {
        error = name + " must be an absolute path";
        return false;
    }
    return true;
}

bool require_directory(
    const std::string& name,
    const std::string& value,
    int access_mode,
    std::string& error
) {
    if (!require_absolute_path(name, value, error)) {
        return false;
    }

    struct stat info;
    if (stat(value.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        error = name + " must reference an existing directory";
        return false;
    }
    if (access(value.c_str(), access_mode) != 0) {
        error = name + " does not have the required process permissions";
        return false;
    }
    return true;
}

}  // namespace

namespace syn_sig_ra {

RuntimeConfig default_runtime_config() {
    RuntimeConfig config;
    config.data_root = SYN_SIG_RA_DEFAULT_DATA_ROOT;
    config.signal_synth_cli = SYN_SIG_RA_DEFAULT_SIGNAL_SYNTH_CLI;
    config.pack_root = SYN_SIG_RA_DEFAULT_PACK_ROOT;
    config.public_base_path = SYN_SIG_RA_DEFAULT_PUBLIC_BASE_PATH;
    return config;
}

bool validate_data_root(const std::string& value, std::string& error) {
    return require_directory(
        "SynSigRaDataRoot",
        value,
        R_OK | W_OK | X_OK,
        error
    );
}

bool validate_signal_synth_cli(
    const std::string& value,
    std::string& error
) {
    if (!require_absolute_path("SynSigRaSignalSynthCli", value, error)) {
        return false;
    }

    struct stat info;
    if (stat(value.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        error =
            "SynSigRaSignalSynthCli must reference an existing regular file";
        return false;
    }
    if (access(value.c_str(), X_OK) != 0) {
        error = "SynSigRaSignalSynthCli must be executable by Apache";
        return false;
    }
    return true;
}

bool validate_pack_root(const std::string& value, std::string& error) {
    return require_directory(
        "SynSigRaPackRoot",
        value,
        R_OK | X_OK,
        error
    );
}

bool validate_public_base_path(
    const std::string& value,
    std::string& error
) {
    const std::string required_prefix("/syn_sig_ra");
    if (value != required_prefix &&
        (value.size() <= required_prefix.size() ||
         value.compare(0, required_prefix.size(), required_prefix) != 0 ||
         value[required_prefix.size()] != '/')) {
        error =
            "SynSigRaPublicBasePath must be /syn_sig_ra or a path below it";
        return false;
    }
    if (value.size() > 1 && value[value.size() - 1] == '/') {
        error = "SynSigRaPublicBasePath must not end with a slash";
        return false;
    }
    if (value.find("//") != std::string::npos ||
        value.find("/../") != std::string::npos ||
        value.find("/./") != std::string::npos ||
        value.find('?') != std::string::npos ||
        value.find('#') != std::string::npos) {
        error = "SynSigRaPublicBasePath is not a normalized URL path";
        return false;
    }
    return true;
}

}  // namespace syn_sig_ra
