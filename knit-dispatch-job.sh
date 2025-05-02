#!/bin/bash -e

. knit-bash-setup

[[ $# -eq 2 ]]
process="$1"
job="$2"

scratch="$KNIT_DIR/scratch/$job"

empty_res() {
    knit-hash-object -t resource -w /dev/null
}

# TODO probably has concurrency bugs when lock file is removed
lock_scratch() {
    if [[ -s "$scratch.lock" ]]; then
        echo "warning: existing lockfile $scratch.lock: $(< $scratch.lock)" >&2
    fi
    exec {lock}> "$scratch.lock"
    flock $lock
    trap 'rm "$scratch.lock"' EXIT
    # TODO handle manual processes
    echo "PID $$" >&$lock
}

unpack_job() {
    if [[ -e $scratch ]]; then
        echo "warning: removing $scratch" >&2
        rm -rf "$scratch"
    fi

    knit-unpack-job --scratch "$job" "$scratch"
}

process_cmd() {
    local rc res
    local -a remix_opts

    set +e
    # TODO disambiguate errors from knit-exec-cmd and .knit/cmd
    knit-exec-cmd "$scratch" "$scratch/work/in/.knit/cmd" {lock}>&- > "$scratch/out.knit/log"
    rc=$?
    set -e

    if [[ -e "$scratch/work/out/.knit" ]]; then
        echo "warning: discarding $scratch/work/out/.knit" >&2
    fi

    remix_opts=(--set-job "$job" --read-outputs-from-dir "$scratch/work/out")
    if [[ -s "$scratch/out.knit/log" ]]; then
        res="$(knit-hash-object -t resource -w "$scratch/out.knit/log")"
        remix_opts+=(--set-output ".knit/log=$res")
    fi
    res="$(knit-hash-object -t resource -w --stdin <<< $rc)"
    remix_opts+=(--set-output ".knit/exitcode=$res")
    if [[ $rc -eq 0 ]]; then
        remix_opts+=(--set-output ".knit/ok=$(empty_res)")
    fi

    knit-remix-production "${remix_opts[@]}"
}

case $process in
    cmd)
        lock_scratch
        unpack_job

        prd=$(process_cmd)

        # TODO when to keep scratch dir?
        rm -rf "$scratch"
        ;;
    flow)
        set -o pipefail
        # By convention, the session name is the same as the flow job; any
        # existing session should be from a previous identical
        # knit-build-session command. This feels a little sloppy but lets us
        # easily resume existing sessions.
        session="$job"
        if [[ -e "$KNIT_DIR/sessions/$session" ]]; then
            echo "warning: existing session $session" >&2
        else
            knit-parse-plan --build-instructions "$job" | knit-build-session "$session"
        fi
        echo session
        exit
        ;;
    identity)
        prd=$(knit-remix-production --set-job "$job" --copy-job-inputs "$job" \
            --remove-prefix .knit/ --set-output ".knit/ok=$(empty_res)")
        ;;
    *)
        echo "Unsupported process $process" >&2
        exit 1
esac

echo "$prd"
