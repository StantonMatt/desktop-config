#!/bin/bash
# Syntax-checks install.sh, rollback.sh, shortcut-lib.sh and the helper test, and runs shellcheck on them when it is
# installed.
set -euo pipefail
dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
scripts=("$dir/install.sh" "$dir/rollback.sh" "$dir/shortcut-lib.sh" "$dir/tests/script-helpers-test.sh")
for script in "${scripts[@]}"; do bash -n "$script"; done
if command -v shellcheck >/dev/null; then
    shellcheck -x -P SCRIPTDIR "${scripts[@]}"
else
    echo 'shellcheck not installed; ran bash -n only'
fi
