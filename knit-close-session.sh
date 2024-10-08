#!/bin/bash -e

. knit-bash-setup

[[ $# -eq 1 ]]
session="$1"

if [[ $(knit-list-steps --available --blocked "$session" | wc -l) -gt 0 ]]; then
    echo Cannot close running session >&2
    exit 1
fi

{
    echo -n 'session '
    knit-hash-object -t resource -w "$KNIT_DIR/sessions/$session"
    echo
    knit-list-steps --porcelain --reverse "$session" | while read -r step state job prd name; do
        echo "$state $job $prd $name"
    done
} | knit-hash-object -t invocation -w --stdin

rm "$KNIT_DIR/sessions/$session"
