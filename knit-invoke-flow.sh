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

[[ $# -eq 1 ]]
flow_job="$1"

set -o pipefail

session="$(knit-parse-plan --job-to-session "$flow_job" | knit-build-session)"
echo "Session $session" >&3

declare -A steps_started
declare -A jobs_dispatched
while [[ $(knit-list-steps --available --blocked "$session" | wc -l) -gt 0 ]]; do
    unset has_scheduled

    while IFS=$'\t' read -r step _ job _ name; do
        [[ -z ${steps_started[$step]} ]] || continue
        steps_started[$step]=1
        has_scheduled=1

        echo "Step $name" >&3

        if prd=$(knit-cache "$job"); then
            echo "Cache hit $job -> $prd" >&3
            knit-complete-job "$session" "$job" "$prd"
        elif [[ -n ${jobs_dispatched[$job]} ]]; then
            echo "Redundant $job" >&3
        else
            echo "Dispatch $job" >&3
            jobs_dispatched[$job]=1
            knit-dispatch-job $verbose "$session" "$job" &
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