#!/bin/bash -e

. knit-bash-setup

if [[ $1 == -v ]]; then
    exec 3>&2
    shift
else
    exec 3> /dev/null
fi

[[ $# -eq 2 ]]
session="$1"
job="$2"

scratch="$KNIT_DIR/scratch/$job"

exec 4> "$scratch.lock"
if ! flock -n 4; then
    echo "Waiting for: $(<$scratch.lock)" >&3
fi
if [[ -s "$scratch.lock" ]]; then
    echo "warning: stale lockfile $scratch.lock: $(<$scratch.lock)" >&2
    truncate -s 0 "$scratch.lock"
fi
# TODO handle manual processes
echo "PID $$ session $session" >&4

if [[ -e $scratch ]]; then
    echo "warning: removing $scratch" >&2
    rm -rf "$scratch"
fi

if prd=$(knit-cache "$job"); then
    echo "Cache hit (locked) $job -> $prd" >&3
    knit-complete-job "$session" "$job" "$prd"
    rm "$scratch.lock"
    exit
fi

knit-unpack-job --scratch "$job" "$scratch"

if [[ -e $scratch/work/in/.knit/cmd ]]; then
    set +e
    # TODO disambiguate errors from knit-exec-cmd and .knit/cmd
    knit-exec-cmd "$scratch" "$scratch/work/in/.knit/cmd" 3>&- 4>&- > "$scratch/out.knit/log"
    rc=$?
    set -e

    if [[ ! -s "$scratch/out.knit/log" ]]; then
        rm "$scratch/out.knit/log"
    fi
    echo $rc > "$scratch/out.knit/exitcode"
    if [[ $rc -eq 0 ]]; then
        touch "$scratch/out.knit/ok"
    fi
elif [[ -e $scratch/work/in/.knit/identity ]]; then
    # TODO implement without unpacking resources
    cp -R "$scratch/work/in/." "$scratch/work/out"
    rm -rf "$scratch/work/out/.knit"
    touch "$scratch/out.knit/ok"
else
    echo "Unsupported job $job" >&2
    exit 1
fi

if [[ -e "$scratch/work/out/.knit" ]]; then
    echo "warning: removing $scratch/work/out/.knit" >&2
    rm -rf "$scratch/work/out/.knit"
fi
mv "$scratch/out.knit" "$scratch/work/out/.knit"

rm -rf "$scratch/work/job"
echo "$job" > "$scratch/work/job"
prd=$(knit-pack-production "$scratch/work")

echo "Complete $job -> $prd" >&3
knit-cache "$job" "$prd"
knit-complete-job "$session" "$job" "$prd"

# TODO when to keep scratch dir?
rm -rf "$scratch"
rm "$scratch.lock"
