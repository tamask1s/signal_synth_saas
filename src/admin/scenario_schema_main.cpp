#include "syn_sig_ra/scenario_schema.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool collect_json_files(
    const std::string& root,
    std::vector<std::string>& files,
    std::string& error
) {
    struct stat information;
    if (lstat(root.c_str(), &information) != 0 || S_ISLNK(information.st_mode)) {
        error = "scenario path is missing or is a symlink";
        return false;
    }
    if (S_ISREG(information.st_mode)) {
        if (ends_with(root, ".json")) files.push_back(root);
        return true;
    }
    if (!S_ISDIR(information.st_mode)) {
        error = "scenario path is not a regular file or directory";
        return false;
    }
    DIR* directory = opendir(root.c_str());
    if (directory == nullptr) {
        error = "unable to open scenario directory";
        return false;
    }
    bool succeeded = true;
    for (dirent* entry = readdir(directory);
         succeeded && entry != nullptr; entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        succeeded = collect_json_files(root + "/" + name, files, error);
    }
    closedir(directory);
    return succeeded;
}

bool read_file(const std::string& path, std::string& value) {
    std::ifstream input(path.c_str(), std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    value = content.str();
    return input.good() || input.eof();
}

bool replace_file(const std::string& path, const std::string& value) {
    struct stat information;
    if (lstat(path.c_str(), &information) != 0 ||
        !S_ISREG(information.st_mode) || S_ISLNK(information.st_mode)) {
        return false;
    }
    std::ostringstream temporary;
    temporary << path << ".schema-v9-" << getpid();
    {
        std::ofstream output(
            temporary.str().c_str(), std::ios::binary | std::ios::trunc);
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        if (!output) {
            std::remove(temporary.str().c_str());
            return false;
        }
    }
    if (chmod(temporary.str().c_str(), information.st_mode & 0777) != 0 ||
        rename(temporary.str().c_str(), path.c_str()) != 0) {
        std::remove(temporary.str().c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--compare-source") {
        std::vector<std::string> files;
        std::string error;
        if (!collect_json_files(argv[3], files, error)) {
            std::cerr << "error=" << error << '\n';
            return 3;
        }
        std::string destination_root(argv[3]);
        if (!destination_root.empty() && destination_root.back() != '/') {
            destination_root += '/';
        }
        for (std::vector<std::string>::const_iterator it = files.begin();
             it != files.end(); ++it) {
            if (it->compare(0, destination_root.size(), destination_root) != 0) {
                std::cerr << "error=destination path escaped its root\n";
                return 4;
            }
            const std::string relative = it->substr(destination_root.size());
            std::string source;
            std::string destination;
            std::string canonical;
            std::vector<std::string> messages;
            if (!read_file(std::string(argv[2]) + "/" + relative, source) ||
                !read_file(*it, destination) ||
                !syn_sig_ra::normalize_current_scenario_json(
                    source, canonical, messages)) {
                std::cerr << "invalid_source=" << relative << '\n';
                return 4;
            }
            canonical += '\n';
            if (canonical != destination) {
                std::cerr << "source_mismatch=" << relative << '\n';
                return 5;
            }
        }
        std::cout << "status=source-aligned files=" << files.size() << '\n';
        return 0;
    }
    if (argc != 3 ||
        (std::string(argv[1]) != "--check" &&
         std::string(argv[1]) != "--apply")) {
        std::cerr << "usage: " << argv[0]
                  << " <--check|--apply> <scenario-file-or-directory>\n"
                  << "       " << argv[0]
                  << " --compare-source <source-directory> <normalized-directory>\n";
        return 2;
    }
    const bool apply = std::string(argv[1]) == "--apply";
    std::vector<std::string> files;
    std::string error;
    if (!collect_json_files(argv[2], files, error)) {
        std::cerr << "error=" << error << '\n';
        return 3;
    }
    unsigned int changed = 0;
    for (std::vector<std::string>::const_iterator it = files.begin();
         it != files.end(); ++it) {
        std::string input;
        std::string canonical;
        std::vector<std::string> messages;
        if (!read_file(*it, input) ||
            !syn_sig_ra::normalize_current_scenario_json(
                input, canonical, messages)) {
            std::cerr << "invalid=" << *it;
            if (!messages.empty()) std::cerr << " message=" << messages[0];
            std::cerr << '\n';
            return 4;
        }
        canonical += '\n';
        if (input == canonical) continue;
        ++changed;
        std::cout << (apply ? "normalized=" : "outdated=") << *it << '\n';
        if (apply && !replace_file(*it, canonical)) {
            std::cerr << "error=unable to replace " << *it << '\n';
            return 5;
        }
    }
    std::cout << "status=" << (apply ? "normalized" : "checked")
              << " files=" << files.size() << " changed=" << changed << '\n';
    return !apply && changed != 0 ? 1 : 0;
}
