#ifndef SYN_SIG_RA_RUNTIME_CONFIG_H
#define SYN_SIG_RA_RUNTIME_CONFIG_H

#include <string>

namespace syn_sig_ra {

struct RuntimeConfig {
    std::string data_root;
    std::string signal_synth_cli;
    std::string pack_root;
    std::string public_base_path;
};

RuntimeConfig default_runtime_config();

bool validate_data_root(const std::string& value, std::string& error);
bool validate_signal_synth_cli(const std::string& value, std::string& error);
bool validate_pack_root(const std::string& value, std::string& error);
bool validate_public_base_path(const std::string& value, std::string& error);

}  // namespace syn_sig_ra

#endif
