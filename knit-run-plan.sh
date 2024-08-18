#!/bin/bash -e

. knit-bash-setup

[[ $# -le 1 ]]
plan="${1-plan.knit}"

set -o pipefail

session="$(knit-parse-plan "$plan" | knit-build-session)"
echo "Session $session" >&2

declare -A started
while ! knit-close-session "$session"; do
    unset has_scheduled

    while IFS=$'\t' read -r step _ job _ name; do
        [[ -z ${started[$step]} ]] || continue
        started[$step]=1
        has_scheduled=1

        echo "Step $name" >&2

        prd=$(knit-check-jobcache "$job" "$session" "$step")
        # TODO crash recovery: restart job
        if [[ $prd == queued ]]; then
            echo "Queued $job [$session $step]" >&2
            continue
        elif [[ $prd == initial ]]; then
            echo "Dispatch $job" >&2
            knit-dispatch-job "$job" &
        else
            echo "Cache hit $job -> $prd" >&2
            knit-fulfill-step "$session" "$step" "$prd"
        fi
    done < <(knit-list-steps --available --porcelain "$session")

    # When we have scheduled everything we can, wait for knit-dispatch-job.
    if [[ -z $has_scheduled ]]; then
        wait -n
    fi
done
