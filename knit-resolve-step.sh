#!/bin/bash -e

set -o pipefail

[[ $# -eq 2 ]]
# TODO session directory
sessiondir="$(<$1)"
step="$2"

find -L "$sessiondir/inputs/$step" -type f | LC_ALL=C sort | while read -r fullpath; do
    filename="${fullpath#$sessiondir/inputs/$step/}"
    printf "%s\0" "$filename"
    knit-hash-object -t resource -w "$fullpath" | printf $(sed 's/../\\x\0/g')
done | knit-hash-object -t job -w --stdin | tee "$sessiondir/resolved/$step"
