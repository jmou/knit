#!/bin/bash -e

. knit-bash-setup

if [[ $1 == -v ]]; then
    exec 3>&2
    shift
else
    exec 3> /dev/null
fi

[[ $# -eq 1 ]]
job_id="$1"

workdir="$KNIT_DIR/workdirs/${job_id:0:2}/${job_id:2}"

# TODO think more carefully about workdir locks; store state in session?
mkdir -p "$(dirname "$workdir")"
exec 4> "$workdir.lock"
if ! flock -n 4; then
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
mkdir "$workdir"
knit-unpack-job "$job_id" "$workdir/root"
mkdir "$workdir/root/out"
mkdir "$workdir/out.knit"

if [[ -e $workdir/root/in/.knit/cmd ]]; then
    set +e
    # TODO disambiguate errors from knit-exec-cmd and .knit/cmd
    knit-exec-cmd "$workdir" "$workdir/root/in/.knit/cmd" 3>&- 4>&- > "$workdir/out.knit/log"
    rc=$?
    set -e

    if [[ ! -s "$workdir/out.knit/log" ]]; then
        rm "$workdir/out.knit/log"
    fi
    echo $rc > "$workdir/out.knit/exitcode"
    if [[ $rc -eq 0 ]]; then
        touch "$workdir/out.knit/ok"
    fi
elif [[ -e $workdir/root/in/.knit/identity ]]; then
    # TODO implement without unpacking resources
    cp -R "$workdir/root/in/." "$workdir/root/out"
    rm -rf "$workdir/root/out/.knit"
    touch "$workdir/out.knit/ok"
else
    echo "Unsupported job $job_id" >&2
    exit 1
fi

if [[ -e "$workdir/root/out/.knit" ]]; then
    echo "warning: removing $workdir/root/out/.knit" >&2
    rm -rf "$workdir/root/out/.knit"
fi
mv "$workdir/out.knit" "$workdir/root/out/.knit"

echo "$job_id" > "$workdir/root/job"
production_id=$(knit-pack-production "$workdir/root")

# TODO when to keep workdirs?
rm -rf "$workdir"

echo "Complete $job_id -> $production_id" >&3
knit-complete-job "$job_id" "$production_id"

rm "$workdir.lock"
