#!/bin/bash -e

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <dir>" >&2
    exit 1
fi

mkdir -p "$1"/.knit/{objects,scratch,sessions}

echo Initialized Knit repository in "$1/.knit" >&2
