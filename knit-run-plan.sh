#!/bin/bash -e

. knit-bash-setup

unset verbose
if [[ $1 == -v ]]; then
    verbose=-v
    exec 3>&2
    shift
else
    exec 3> /dev/null
fi

[[ $# -le 1 ]]
plan="${1-plan.knit}"

set -o pipefail

session="$(knit-parse-plan "$plan" | knit-build-session)"
echo "Session $session" >&3

declare -A started
while [[ $(knit-list-steps --available --blocked "$session" | wc -l) -gt 0 ]]; do
    unset has_scheduled

    while IFS=$'\t' read -r step _ job _ name; do
        [[ -z ${started[$step]} ]] || continue
        started[$step]=1
        has_scheduled=1

        echo "Step $name" >&3

        prd=$(knit-check-jobcache "$job" "$session" "$step")
        # TODO crash recovery: restart job
        if [[ $prd == queued ]]; then
            echo "Queued $job [$session $step]" >&3
            continue
        elif [[ $prd == initial ]]; then
            echo "Dispatch $job" >&3
            knit-dispatch-job $verbose "$job" &
        else
            echo "Cache hit $job -> $prd" >&3
            knit-fulfill-step "$session" "$step" "$prd"
        fi
    done < <(knit-list-steps --available --porcelain "$session")

    # When we have scheduled everything we can, wait for knit-dispatch-job.
    if [[ -z $has_scheduled ]]; then
        wait -n
    fi
done

num_unmet=$(knit-list-steps --unmet "$session" | wc -l)
if [[ $num_unmet -gt 0 ]]; then
    [[ $num_unmet -eq 1 ]] || plural=s
    echo "$num_unmet step$plural with unmet requirements:" >&2
    knit-list-steps --unmet "$session" >&2
fi

knit-close-session "$session"
