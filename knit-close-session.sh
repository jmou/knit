#!/bin/bash -e

[[ $# -eq 1 ]]
# TODO session directory
session="$1"
sessiondir="$(<$1)"

cmp -s <(knit-list-steps --all "$session" | sort) <({ knit-list-steps --recorded "$session"; knit-list-steps --unresolvable "$session"; } | sort)

if [[ -n $(knit-list-steps --unresolvable "$session") ]]; then
    echo 'Some steps were unresolvable:' >&2
    knit-list-steps --unresolvable "$session" >&2
fi

# TODO invocation id
echo "$sessiondir/productions"
