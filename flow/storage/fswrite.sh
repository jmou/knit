#!/bin/bash -e

fsroot="$(<in/fsroot)"
source="${1-in/source}"

mkdir -p "$fsroot"
path="$fsroot/$(sha1sum "$source" | cut -d' ' -f1)"
cp "$source" "$path"
echo -e "path=$path\nstore=fs"
