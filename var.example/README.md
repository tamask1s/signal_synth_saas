# Local runtime layout

Production deployments use the following default layout:

```text
/var/lib/syn_sig_ra/
  db.sqlite3
  jobs/
  work/
  packages/
```

Create it before enabling the Apache configuration and grant ownership only to
the account that runs the Apache module and worker:

```sh
sudo install -d -o www-data -g www-data -m 0750 \
  /var/lib/syn_sig_ra \
  /var/lib/syn_sig_ra/jobs \
  /var/lib/syn_sig_ra/work \
  /var/lib/syn_sig_ra/packages
```

Do not commit a populated runtime tree, database, generated package, or API
key. CMake creates the equivalent disposable development layout below the
selected build directory.
