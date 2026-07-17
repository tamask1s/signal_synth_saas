#include "syn_sig_ra/metadata_store.h"
#include "syn_sig_ra/sha256.h"

#include <sqlite3.h>
#include <unistd.h>

#include <cstdio>
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

void create_sqlite(const std::string& path, const char* sql) {
    sqlite3* database = nullptr;
    require(sqlite3_open(path.c_str(), &database) == SQLITE_OK,
        "fixture database should open");
    require(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK,
        "fixture SQL should run");
    sqlite3_close(database);
}

}  // namespace

int main() {
    const std::string base =
        "/tmp/syn_sig_ra_schema_" + std::to_string(getpid());
    std::string error;

    const std::string obsolete = base + "_obsolete.sqlite3";
    std::remove(obsolete.c_str());
    create_sqlite(obsolete, "PRAGMA user_version=10; CREATE TABLE old_data(id);");
    syn_sig_ra::MetadataStore obsolete_store(obsolete);
    require(!obsolete_store.initialize(error) &&
        error.find("destructive pre-beta reset") != std::string::npos,
        "old versioned databases must require a reset");

    const std::string unversioned = base + "_unversioned.sqlite3";
    std::remove(unversioned.c_str());
    create_sqlite(unversioned, "CREATE TABLE old_data(id);");
    syn_sig_ra::MetadataStore unversioned_store(unversioned);
    error.clear();
    require(!unversioned_store.initialize(error) &&
        error.find("destructive pre-beta reset") != std::string::npos,
        "nonempty unversioned databases must require a reset");

    const std::string fresh = base + "_fresh.sqlite3";
    std::remove(fresh.c_str());
    syn_sig_ra::MetadataStore store(fresh);
    require(store.initialize(error), "fresh schema should initialize: " + error);
    syn_sig_ra::ApiKeyIdentity owner;
    owner.organization_id = "org_bootstrap";
    owner.user_id = "user_bootstrap";
    owner.api_key_id = "key_bootstrap";
    owner.role = "owner";
    std::string hash;
    require(syn_sig_ra::sha256_hex("bootstrap-secret", hash, error),
        "bootstrap secret should hash");
    require(store.bootstrap_owner(
        owner, "owner@example.test", "Owner", hash, "bootstrap", error),
        "explicit owner bootstrap should succeed: " + error);
    require(!store.bootstrap_owner(
        owner, "owner@example.test", "Owner", hash, "bootstrap", error),
        "owner bootstrap must not silently overwrite state");
    syn_sig_ra::ApiKeyIdentity missing = owner;
    missing.api_key_id = "key_missing";
    missing.user_id = "user_missing";
    require(!store.create_api_key(missing, hash, "missing", error),
        "create-api-key must not fabricate users");

    std::remove(obsolete.c_str());
    std::remove(unversioned.c_str());
    std::remove(fresh.c_str());
    return EXIT_SUCCESS;
}
