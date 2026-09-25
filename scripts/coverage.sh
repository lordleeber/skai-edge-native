#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if ! command -v gcovr >/dev/null 2>&1; then
    echo "gcovr is required to generate the coverage report" >&2
    exit 1
fi

cmake --preset x86-coverage
cmake --build --preset x86-coverage
ctest --preset x86-coverage

report_dir="$repo_root/build/x86-coverage/report"
mkdir -p "$report_dir"
gcovr_args=(
    --root .
    --gcov-exclude-directory '.*CompilerId.*'
    --gcov-exclude-directory '.*/third_party/.*'
    --gcov-ignore-parse-errors negative_hits.warn_once_per_file
)

gcovr "${gcovr_args[@]}" \
    --filter 'src/' --filter 'include/skai/' \
    --txt "$report_dir/coverage.txt" \
    --html-details "$report_dir/index.html" \
    --xml "$report_dir/coverage.xml" \
    build/x86-coverage

gcovr "${gcovr_args[@]}" \
    --filter 'src/(cli\.cpp|inference/(tensor_layout|preprocess|yolo_postprocess)\.cpp)$' \
    --json-summary "$report_dir/pure-logic-summary.json" \
    build/x86-coverage

python3 - "$report_dir/pure-logic-summary.json" <<'PY'
import json
import sys

expected = {
    "src/cli.cpp",
    "src/inference/preprocess.cpp",
    "src/inference/tensor_layout.cpp",
    "src/inference/yolo_postprocess.cpp",
}
minimum = 90.0
with open(sys.argv[1], encoding="utf-8") as report:
    files = {item["filename"]: item for item in json.load(report)["files"]}

failed = False
for name in sorted(expected):
    item = files.get(name)
    if item is None or item["line_total"] == 0:
        print(f"{name}: no coverage data")
        failed = True
        continue
    percent = 100.0 * item["line_covered"] / item["line_total"]
    print(f"{name}: {percent:.1f}% line coverage (minimum {minimum:.0f}%)")
    failed |= percent < minimum
if failed:
    sys.exit(1)
PY

echo "Coverage reports: $report_dir"
