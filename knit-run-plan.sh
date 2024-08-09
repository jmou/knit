#!/bin/bash -e

. knit-bash-setup

[[ $# -le 1 ]]
plan="${1-plan.knit}"

session="$(knit-parse-plan "$plan")"
echo "Session $session" >&2

# Keep resolvable steps in positional args.
set --
while true; do
    if [[ $# -eq 0 ]]; then
        if knit-close-session "$session"; then
            break
        fi
        set -- $(knit-list-steps --resolvable "$session")
        if [[ $# -eq 0 ]]; then
            wait -n  # wait for dispatch-job
            continue
        fi
        step="$1"
        shift
    fi

    echo "Step $step" >&2

    job_id=$(knit-resolve-step "$session" "$step")

    production_id=$(knit-check-jobcache "$job_id" "$session" "$step")
    # TODO crash recovery: restart job
    if [[ $production_id == queued ]]; then
        echo "Queued $job_id [$session $step]" >&2
        continue
    elif [[ $production_id == initial ]]; then
        echo "Dispatch $job_id" >&2
        knit-dispatch-job "$job_id" &
    else
        echo "Cache hit $job_id -> $production_id" >&2
        knit-fulfill-step "$session" "$step" "$production_id"
    fi
done
