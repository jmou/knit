#!/bin/bash -e

# Abuse git by storing any object as a blob.
if [[ $1 == -t ]]; then
    set -- -t blob "${@:3}"
fi

GIT_DIR="$KNIT_DIR" exec git hash-object "$@"
