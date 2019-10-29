#!/bin/bash -e

: ${input_fsroot?}
source=${1-${input_source?}}

mkdir -p "$(<$input_fsroot)"
path="$(<$input_fsroot)/$(sha1sum "$source" | cut -d' ' -f1)"
cp "$source" "$path"
echo -e "path=$path\nstore=fs"
