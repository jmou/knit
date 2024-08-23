#!/bin/bash -e

. knit-bash-setup

unset verbose
if [[ $1 == -v ]]; then
    verbose=-v
    exec 3>&2
    shift
else
    exec 3> /dev/null
fi

[[ $# -eq 2 ]]
session="$1"
job="$2"

scratch="$KNIT_DIR/scratch/$job"

lock_scratch() {
    if [[ -s "$scratch.lock" ]]; then
        echo "warning: existing lockfile $scratch.lock: $(< $scratch.lock)" >&2
    fi
    exec 4> "$scratch.lock"
    flock 4
    trap 'rm "$scratch.lock"' EXIT
    # TODO handle manual processes
    echo "PID $$ session $session" >&4
}

unpack_job() {
    if [[ -e $scratch ]]; then
        echo "warning: removing $scratch" >&2
        rm -rf "$scratch"
    fi

    knit-unpack-job --scratch "$job" "$scratch"
}

process_cmd() {
    local rc

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

    if [[ -e "$scratch/work/out/.knit" ]]; then
        echo "warning: removing $scratch/work/out/.knit" >&2
        rm -rf "$scratch/work/out/.knit"
    fi
    mv "$scratch/out.knit" "$scratch/work/out/.knit"

    rm -rf "$scratch/work/job"
    echo "$job" > "$scratch/work/job"
    knit-pack-production "$scratch/work"
}

finish() {
    echo "Complete $job -> $prd" >&3
    knit-cache "$job" "$prd"
    knit-complete-job "$session" "$job" "$prd"
    exit
}

unset prd
while read -r input; do
    if [[ $input == .knit/cmd ]]; then
        lock_scratch
        if prd=$(knit-cache "$job"); then
            echo "Cache hit (locked) $job -> $prd" >&3
            knit-complete-job "$session" "$job" "$prd"
            rm "$scratch.lock"
            exit
        fi
        unpack_job

        prd=$(process_cmd)

        # TODO when to keep scratch dir?
        rm -rf "$scratch"
        finish
    elif [[ $input == .knit/flow ]]; then
        inv=$(knit-invoke-flow $verbose "$job")
        prd=$(knit-remix-production --set-job "$job" --wrap-invocation "$inv")
        finish
    elif [[ $input == .knit/identity ]]; then
        empty_res=$(knit-hash-object -t resource -w /dev/null)
        prd=$(knit-remix-production --set-job "$job" --copy-job-inputs "$job" \
            --remove-prefix .knit/ --set-output ".knit/ok=$empty_res")
        finish
    fi
done < <(knit-cat-file -p "$job" | cut -f2-)

echo "Unsupported job $job" >&2
exit 1
