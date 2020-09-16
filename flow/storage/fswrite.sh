#!/bin/bash -e

fsroot="$(<in/fsroot)"
source="${1-in/source}"

mkdir -p "$fsroot"
path="$fsroot/$(openssl dgst -sha1 -binary "$source" | xxd -p)"
cp "$source" "$path"
echo -e "path=$path\nstore=fs"
