#!/bin/bash -e

# Assume all objects are actually git blobs; see hash-object.
if [[ $1 != -* ]]; then
    shift
    set -- blob "$@"
fi
GIT_DIR="$KNIT_DIR" exec git cat-file "$@"
