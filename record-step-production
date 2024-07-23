#!/bin/bash -e

[[ $# -eq 3 ]]
# TODO session directory
sessiondir="$(<$1)"
step="$2"
# TODO production directory
productiondir="$3"

ln -sT "$PWD/$productiondir" "$sessiondir/productions/$step"
