#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
legacy_pattern='SynSigRa([^A-Z]|$)|Sinsigra|Syn_sig_ra'
files='
README.md
src/api/route.cpp
src/email/transactional_email.cpp
src/mod_syn_sig_ra.cpp
doc/openapi.yaml
doc/SAAS_PRODUCT_PLAN.md
doc/VPS_DEPLOYMENT.md
doc/PRIVATE_BETA_TERMS.md
doc/PRIVACY_NO_PHI_NOTICE.md
doc/PRIVATE_BETA_SUPPORT.md
doc/PRODUCT_CAPABILITIES.md
apache/syn_sig_ra.conf.example
ops/mail/README.md
ops/mail/configure_gmail_smtp.sh
scripts/build_verifier_downloads.sh
scripts/customer_smoke.py
scripts/stress_live_packs.py
scripts/verify_downloaded_package.py
scripts/mail/README.md
scripts/mail/install_local_mta.sh
scripts/mail/verify_local_mta.sh
'

failed=0
for relative in $files; do
    if grep -nE "$legacy_pattern" "$repo_dir/$relative"; then
        failed=1
    fi
done

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
unzip -qq "$repo_dir/doc/synsigra_main_landing_package_v10.zip" -d "$tmp_dir"
if grep -RInE "$legacy_pattern" "$tmp_dir" --exclude='*.png'; then
    failed=1
fi

landing="$tmp_dir/synsigra_main_landing_package/main/index.html"
grep -Eq "0\.01 (seconds|s).*(24 hours|24 h)" "$landing" ||
    { echo "landing capability range is missing" >&2; failed=1; }
if grep -qiE "new front door|landing page is|page intentionally presents" "$landing"; then
    echo "internal landing-page copy found in customer-facing content" >&2
    failed=1
fi
grep -q 'mailto:synsigra@gmail.com?subject=Synsigra%20technical%20demo' "$landing" ||
    { echo "landing technical-demo CTA is missing" >&2; failed=1; }
grep -q 'Kis Tamás' "$landing" ||
    { echo "landing operator identity is missing" >&2; failed=1; }
grep -q '2040 Budaörs, Tátra u. 6, Hungary' "$landing" ||
    { echo "landing operator address is missing" >&2; failed=1; }

if [ "$failed" -ne 0 ]; then
    echo "legacy product spelling found in user-facing content" >&2
    exit 1
fi
