#!/bin/bash -e

[[ $# -eq 2 ]]
job_id="$1"
production_id="$2"

cache="$KNIT_DIR/jobcache/${job_id:0:2}/${job_id:2}"
mkdir -p "$(dirname "$cache")"
exec 3> "$cache.lock"
flock 3
trap 'rm "$cache.lock"' EXIT

if [[ -e $cache ]]; then
    echo "Job $job_id has already been completed with production $(<$cache)" >&2
    exit 1
fi

echo "$production_id" > $cache.tmp
mv $cache.tmp $cache

[[ -e $cache.pending ]] || exit
# TODO possible to orphan pending sessions if we crash
# TODO drop the lock?
while IFS=$'\t' read -r session step; do
    knit-record-step-production "$session" "$step" "$production_id"
done < $cache.pending
rm $cache.pending
