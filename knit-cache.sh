#!/bin/bash -e

. knit-bash-setup

unset prd
if [[ $# -eq 2 ]]; then
    prd="$2"
elif [[ $# -ne 1 ]]; then
    echo "usage: $0 <job> [<production>]" >&2
    exit 1
fi
job="$1"

cache="$KNIT_DIR/cache/${job:0:2}/${job:2}"

# As an optimization attempt to read the cache outside of the lock. This
# should be safe because we assume the cache is set once atomically.
if [[ -z $prd && -e $cache ]]; then
    echo "$(< "$cache")"
    exit
fi

mkdir -p "$(dirname "$cache")"
exec 3> "$cache.lock"
flock 3

rc=0
if [[ -z $prd ]]; then
    if [[ -e $cache ]]; then
        echo "$(< "$cache")"
    else
        rc=1
    fi
else
    if [[ -e $cache ]]; then
        existing="$(< $cache)"
        if [[ $existing == $prd ]]; then
            echo "warning: cache redundantly set" >&2
        else
            echo "Job already cached with production $existing" >&2
            rc=1
        fi
    else
        echo "$prd" > $cache.tmp
        mv $cache.tmp $cache
    fi
fi

rm "$cache.lock"
exit "$rc"
