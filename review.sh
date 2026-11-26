#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
mkdir -p .review/snapshots
exec > >(tee -a .review/operations.log) 2>&1
printf '\n[%s] Incremental source review\n' "$(date -Is)"
for file in "$@"; do
    [[ $file != /* && $file != *..* && -f $file ]] || exit 1
    printf '\nFILE: %s\n' "$file"
    previous=.review/snapshots/$file
    if [[ -f $previous ]]; then
        cat -- "$file"
        printf '\nINCREMENTAL DIFF: %s\n' "$file"
        diff -u --label "previous/$file" --label "current/$file" "$previous" "$file" || [[ $? == 1 ]]
    else
        printf 'NEW SOURCE / COMPLETE ADDITION DIFF: %s\n' "$file"
        diff -u --label /dev/null --label "$file" /dev/null "$file" || [[ $? == 1 ]]
    fi
    mkdir -p -- "$(dirname -- "$previous")"
    cp -- "$file" "$previous"
done
