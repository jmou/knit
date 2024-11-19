#!/bin/bash -e

. knit-bash-setup

[[ $# -eq 1 ]]
job="$1"

scratch="$KNIT_DIR/scratch/$job"

empty_res() {
    knit-hash-object -t resource -w /dev/null
}

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

finish() {
    if [[ -z $nocache ]]; then
        knit-cache "$job" "$prd"
    fi
    echo "$prd"
    exit
}

unset prd
if prd=$(knit-cache "$job"); then
    echo -e "!!cache-hit\t$job\t$prd" >&2
    echo "$prd"
    exit
fi

# This quick-and-dirty invocation of knit-cat-file could be optimized away, but
# it would require more lines of code :)
unset nocache
if knit-cat-file -p "$job" | cut -f2- | grep -qxF .knit/nocache; then
    nocache=1
fi

while read -r input; do
    if [[ $input == .knit/cmd ]]; then
        lock_scratch
        if prd=$(knit-cache "$job"); then
            echo -e "!!cache-hit-slow\t$job\t$prd" >&2
            rm "$scratch.lock"
            echo "$prd"
            exit
        fi
        unpack_job

        prd=$(process_cmd)

        # TODO when to keep scratch dir?
        rm -rf "$scratch"
        finish
    elif [[ $input == .knit/flow ]]; then
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
        inv=$(knit-resume-session "$session")
        prd=$(knit-remix-production --set-job "$job" --wrap-invocation "$inv")
        finish
    elif [[ $input == .knit/identity ]]; then
        prd=$(knit-remix-production --set-job "$job" --copy-job-inputs "$job" \
            --remove-prefix .knit/ --set-output ".knit/ok=$(empty_res)")
        finish
    fi
done < <(knit-cat-file -p "$job" | cut -f2-)

echo "Unsupported job $job" >&2
exit 1
