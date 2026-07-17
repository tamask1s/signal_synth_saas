# Local runtime layout

Production deployments use the following default layout:

```text
/var/lib/syn_sig_ra/
  db.sqlite3
  work/
  packages/
  recipes/
  generator_releases/
  custom_packs/
  derived-artifacts/
```

Create it before enabling the Apache configuration and grant ownership only to
the account that runs the Apache module and worker:

```sh
sudo install -d -o www-data -g www-data -m 0750 \
  /var/lib/syn_sig_ra \
  /var/lib/syn_sig_ra/work \
  /var/lib/syn_sig_ra/packages \
  /var/lib/syn_sig_ra/recipes \
  /var/lib/syn_sig_ra/generator_releases \
  /var/lib/syn_sig_ra/custom_packs \
  /var/lib/syn_sig_ra/derived-artifacts
```

Do not commit a populated runtime tree, database, generated package, or API
key. CMake creates the equivalent disposable development layout below the
selected build directory.
