#include "syn_sig_ra/runtime_config.h"

#include <cstdlib>
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
    std::string error;
    const syn_sig_ra::RuntimeConfig defaults =
        syn_sig_ra::default_runtime_config();

    require(
        syn_sig_ra::validate_data_root(defaults.data_root, error),
        "the generated development data root should be valid: " + error
    );
    error.clear();
    require(
        syn_sig_ra::validate_pack_root(defaults.pack_root, error),
        "the source pack root should be valid: " + error
    );
    error.clear();
    require(
        syn_sig_ra::validate_signal_synth_cli("/bin/sh", error),
        "an existing executable should be a valid CLI path: " + error
    );
    error.clear();
    require(
        !syn_sig_ra::validate_signal_synth_cli(
            "/definitely/missing/signal-synth",
            error
        ),
        "a missing CLI path should be rejected"
    );
    error.clear();
    require(
        syn_sig_ra::validate_public_base_path("/syn_sig_ra", error),
        "the default public base path should be valid"
    );
    error.clear();
    require(
        syn_sig_ra::validate_public_base_path(
            "/syn_sig_ra/internal",
            error
        ),
        "a normalized child path should be valid"
    );
    error.clear();
    require(
        !syn_sig_ra::validate_public_base_path("/other", error),
        "a base path outside /syn_sig_ra should be rejected"
    );
    error.clear();
    require(
        !syn_sig_ra::validate_public_base_path("/syn_sig_ra/", error),
        "a trailing slash should be rejected"
    );

    return EXIT_SUCCESS;
}
