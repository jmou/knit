#!/bin/bash -e

[[ $# -eq 3 ]]
# TODO session directory
sessiondir="$(<$1)"
step="$2"
production_id="$3"

knit-cat-file -p "$production_id" | sed '1,/^$/d' | while IFS=$'\t' read -r resource name; do
    path="$sessiondir/productions/$step/$name"
    mkdir -p "$(dirname "$path")"
    knit-cat-file res "$resource" > "$path"
done
