#!/bin/bash -e

. knit-bash-setup

empty_res() {
    knit-hash-object -t resource -w /dev/null
}

cmd=(knit-remix-production)
unset fail
while [[ $# -gt 0 ]]; do
    case $1 in
        --job) cmd+=(--set-job "$2"); shift 2;;
        --fail) fail=1; shift;;
        --file)
            res="$(knit-hash-object -t resource -w "$2")"
            cmd+=(--set-output "${2#*/./}=$res")
            shift 2
            ;;
        --directory) cmd+=(--read-outputs-from-dir "$2"); shift 2;;
        *)
            echo "Unhandled option $1" >&2
            exit 1
            ;;
    esac
done
if [[ -z $fail ]]; then
    cmd+=(--set-output ".knit/ok=$(empty_res)")
fi

"${cmd[@]}" > "$KNIT_DIR/run.pipe"
