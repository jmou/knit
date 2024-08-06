#!/bin/bash -e

[[ $# -eq 1 ]]
job_id="$1"

workdir="$KNIT_DIR/workdirs/${job_id:0:2}/${job_id:2}"

# TODO think more carefully about workdir locks; store state in session?
mkdir -p "$(dirname "$workdir")"
exec 3> "$workdir.lock"
if ! flock -n 3; then
    echo "Job already running: $(<$workdir.lock)" >&2
    exit 1
fi
if [[ -s "$workdir.lock" ]]; then
    echo "Found stale lockfile $workdir.lock: $(<$workdir.lock)" >&2
    truncate -s 0 "$workdir.lock"
fi
# TODO handle manual processes
echo PID $$ > "$workdir.lock"

rm -rf "$workdir"
knit-unpack-job "$job_id" "$workdir"
mkdir "$workdir/out"

cd "$workdir"
# TODO keep special .knit directory out of workdir
mkdir out/.knit

set +e
$SHELL -e in/shell <&- 3>&- &> out/.knit/log
rc=$?
set -e

if [[ ! -s out/.knit/log ]]; then
    rm out/.knit/log
fi
echo $rc > out/.knit/exitcode
if [[ $rc -eq 0 ]]; then
    touch out/.knit/ok
fi

cd "$OLDPWD"
production_id=$(knit-assemble-production "$job_id" "$workdir/out")
# TODO when to keep workdirs?
rm -rf "$workdir"
knit-complete-job "$job_id" "$production_id"

rm "$workdir.lock"

echo "Complete $job_id -> $production_id" >&2
