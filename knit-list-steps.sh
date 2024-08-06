#!/bin/bash -e

shopt -s nullglob

[[ $# -eq 2 ]]
flag="$1"
# TODO session directory
sessiondir="$(<$2)"

valid_symlinks() {
    local dir="$1"
    for input in "$dir/"*; do
        [[ -e $input ]] || return 1
    done
}

list_with() {
    cd "$sessiondir/$1"
    ls -1
    cd $OLDPWD
}

state_blocked() {
    local step="$1"
    ! valid_symlinks "$sessiondir/awaiting/$step"
}

state_unresolvable() {
    local step="$1"
    ! state_blocked "$step" && ! valid_symlinks "$sessiondir/inputs/$step"
}

is_resolved() {  # prepared or recorded
    local step="$1"
    [[ -e "$sessiondir/resolved/$step" ]]
}

# Settle unresolvable jobs.
settling=1
while [[ -n $settling ]]; do
    unset settling
    for step in $(list_with inputs); do
        if [[ ! -e "$sessiondir/productions/$step" ]] && state_unresolvable "$step"; then
            mkdir -p "$sessiondir/productions/$step"
            settling=1
        fi
    done
done

if [[ $flag == --all ]]; then
    list_with inputs
elif [[ $flag == --available ]]; then
    for step in $(list_with inputs); do
        if ! state_blocked "$step" && ! is_resolved "$step" && valid_symlinks "$sessiondir/inputs/$step"; then
            echo "$step"
        fi
    done
elif [[ $flag == --recorded ]]; then
    for step in $(list_with productions); do
        if ! state_unresolvable "$step"; then
            echo "$step"
        fi
    done
elif [[ $flag == --unresolvable ]]; then
    for step in $(list_with inputs); do
        if state_unresolvable "$step"; then
            echo "$step"
        fi
    done
else
    echo "Unrecognized flag $flag" >&2
    exit 1
fi
