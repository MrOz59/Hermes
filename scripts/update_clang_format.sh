#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
find src tests tools -type f \
    \( -name '*.cpp' -o -name '*.cppm' -o -name '*.cu' -o -name '*.h' -o -name '*.hpp' -o -name '*.m' -o -name '*.mm' \) \
    -print0 |
    while IFS= read -r -d '' file; do
        printf 'Formatting %s ...\n' "$file"
        clang-format -i "$file"
    done
