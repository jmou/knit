#!/bin/bash -e

[[ $# -eq 3 ]]
job_id="$1"
session="$2"
step="$3"

cache="$KNIT_DIR/jobcache/${job_id:0:2}/${job_id:2}"

# As an optimization attempt to read the cache outside of the lock. This
# should be safe because we assume the cache is set once atomically.
if [[ -e $cache ]]; then
    echo "$(<"$cache")"
    exit
fi

mkdir -p "$(dirname "$cache")"
exec 3> "$cache.lock"
flock 3

if [[ -e $cache ]]; then
    echo "$(<"$cache")"
    exit
fi

if [[ -e $cache.pending ]]; then
    cp "$cache.pending" "$cache.pending.tmp"
    echo queued
else
    echo initial
fi

# TODO make safe for tabs in session or newlines in step?
echo "$session"$'\t'"$step" >> "$cache.pending.tmp"
mv "$cache.pending.tmp" "$cache.pending"

rm "$cache.lock"
