#!/bin/bash -e

. knit-bash-setup.sh

if [[ $# -gt 1 ]]; then
    echo "usage: $0 <dir>" >&2
    exit 1
fi

if [[ $# -eq 1 && $KNIT_DIR != /* ]]; then
    KNIT_DIR="$1/$KNIT_DIR"
fi

mkdir -p "$KNIT_DIR"/{objects,scratch,sessions}

echo Initialized Knit repository in "$KNIT_DIR" >&2
