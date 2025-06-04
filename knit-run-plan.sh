#!/bin/bash -e

. knit-bash-setup

usage() { echo "usage: $0 [-f <plan>] [--no-filter] [<plan-job-options>]" >&2; exit 1; }

if [[ -t 1 ]]; then
    filter=knit-filter-status
else
    filter='grep -v ^!!'
fi

plan=plan.knit
while [[ $# -gt 0 ]]; do
    case "$1" in
        -f) plan="$2"; shift;;
        --no-filter) filter=cat;;
        *) break;;
    esac
    shift
done

set -o pipefail

job=$(knit-parse-plan --emit-params-files "$plan" | knit-plan-job "$@" "$plan")

if [[ ! -p "$KNIT_DIR/run.pipe" ]]; then
    mkfifo "$KNIT_DIR/run.pipe"
fi

# TODO process substitution obscures any failure exit status
while read -r word oid; do
    if [[ $word == ok ]]; then
        prd="$oid"
    elif [[ $word == external ]]; then
        echo "External job $oid" >&2
    else
        echo "Unrecognized word $word" >&2
    fi
# Open run.pipe for read/write to prevent waiting for writer.
# See https://unix.stackexchange.com/a/496812
# We do some crazy file descriptor juggling to filter stderr.
# See https://stackoverflow.com/a/52575213/13773246
# The `|| true` prevents grep's exit code from terminating the entire script.
done < <({ knit-schedule-jobs "$job" 1<> "$KNIT_DIR/run.pipe" 2>&1 >&3 3>&- | { $filter || true; } >&2 3>&-; } 3>&1)

# TODO truncate history
echo "$prd" | tee -a "$KNIT_DIR/history"

# TODO surface diagnostics for subflows
# TODO diagnostic if missing .knit/ok
# TODO unmet requirements should indicate missing dependency and how to inspect
# TODO should invocation store session?
session=$(knit-cat-file -p $prd^{invocation} | grep '^session ' | cut -d' ' -f2)
num_unmet=$(knit-list-steps --unmet <(knit-cat-file -p $session) | wc -l)
if [[ $num_unmet -gt 0 ]]; then
    [[ $num_unmet -eq 1 ]] || plural=s
    echo "$num_unmet step$plural with unmet requirements:" >&2
    knit-list-steps --unmet <(knit-cat-file -p $session) >&2
fi
