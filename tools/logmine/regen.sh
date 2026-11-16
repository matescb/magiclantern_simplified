#!/bin/sh
# Regenerate the C7 tables and report.  Read-only on tools/mlserial/run/*.
set -e
cd "$(dirname "$0")"
python3 test_logmine.py
python3 logmine.py "$@" > out_summary.txt
REPORT=${REPORT:-../../docs/offline/C7.md}
{
  cat report_head.md
  echo "### (1) Abort signatures"; echo; sed 's/^## /#### /; s/^### /##### /' out/crashes.md
  echo; echo "### (2) Canon asserts"; echo; sed 's/^## /#### /' out/asserts.md
  echo; echo "### (3) Per-build reliability"; echo; sed 's/^## /#### /' out/builds.md
  echo; echo "### (4) Recording attempts"; echo; sed 's/^## /#### /' out/recordings.md
  echo; echo "### (5) Copy and write rates"; echo; sed 's/^## /#### /' out/rates.md
  cat report_tail.md
} > "$REPORT"
echo "wrote $REPORT"
