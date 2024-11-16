#!/bin/bash -e

. knit-bash-setup

usage() { echo "usage: $0 [-v] [-f <plan>] [<plan-job-options>]" >&2; exit 1; }

plan=plan.knit
unset verbose
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v) verbose=-v;;
        -f) plan="$2"; shift;;
        *) break;;
    esac
    shift
done

set -o pipefail

job=$(knit-parse-plan --emit-params-files "$plan" | knit-plan-job "$@" "$plan")
prd=$(knit-dispatch-job $verbose "$job")
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
