#!/usr/bin/env bash
# Fails when a tracked file mentions ArmarX, the ARMAR robots, Axii, H2T/KIT or an internal path or user name.
# Exceptions: one extended regular expression per line in tools/reference_allowlist.txt, matched against the
# git grep line "path:line:text" (or "Binary file path matches"). Run from anywhere inside the repository.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
PATTERN='armarx|armar[67]|axii|h2t|kit\.edu|/cvhci|/lsdf|/var/tmp|armar-user|visionx|armem|dschneider'
ALLOWLIST=tools/reference_allowlist.txt
[ -f "$ALLOWLIST" ] || { echo "missing $ALLOWLIST" >&2; exit 2; }
hits="$(git grep -n -i -E "$PATTERN" -- . ":!tools/check_references.sh" ":!$ALLOWLIST" || true)"
allowed="$(grep -v -E '^[[:space:]]*(#|$)' "$ALLOWLIST" || true)"
if [ -n "$allowed" ] && [ -n "$hits" ]; then
    hits="$(printf '%s\n' "$hits" | grep -v -E -f <(printf '%s\n' "$allowed") || true)"
fi
if [ -n "$hits" ]; then
    printf '%s\n' "$hits"
    echo "check_references: $(printf '%s\n' "$hits" | wc -l) line(s) with internal references (see above)" >&2
    exit 1
fi
echo "check_references: no internal references"
