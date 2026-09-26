#!/usr/bin/env bash

set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repository_root"

if [[ -n "${SKAI_GITHUB_TOKEN:-}" ]]; then
    auth_value=$(printf 'x-access-token:%s' "$SKAI_GITHUB_TOKEN" | base64 | tr -d '\n')
    git -c "http.extraHeader=Authorization: Basic $auth_value" \
        submodule update --init --recursive third_party/skai-ice
else
    if ! git submodule update --init --recursive third_party/skai-ice; then
        printf '%s\n' \
            'Unable to fetch the private skai-ice dependency.' \
            'Set SKAI_GITHUB_TOKEN to a GitHub token with read access and retry.' >&2
        exit 1
    fi
fi

git submodule update --init third_party/libdatachannel
git submodule update --init --recursive third_party/libdatachannel

expected_skai_ice=$(git ls-tree HEAD third_party/skai-ice | awk '{print $3}')
expected_libdatachannel=$(git ls-tree HEAD third_party/libdatachannel | awk '{print $3}')
actual_skai_ice=$(git -C third_party/skai-ice rev-parse HEAD)
actual_libdatachannel=$(git -C third_party/libdatachannel rev-parse HEAD)

if [[ "$actual_skai_ice" != "$expected_skai_ice" ||
      "$actual_libdatachannel" != "$expected_libdatachannel" ]]; then
    printf '%s\n' 'Dependency checkout does not match the pinned gitlinks.' >&2
    exit 1
fi

printf 'Dependencies ready: skai-ice %s, libdatachannel %s\n' \
    "${actual_skai_ice:0:7}" "${actual_libdatachannel:0:7}"
