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

# TODO cache top-level flow jobs
exec knit-invoke-flow $verbose "$job"
