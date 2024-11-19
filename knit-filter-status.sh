#!/bin/bash

. knit-bash-setup

round1() {
    # Fixed point arithmetic with one decimal position.
    local div1=$(( $2 / 10 ))
    local round1=$(( ($1 + ($div1 / 2)) / $div1 ))
    # This printf incantation helpfully formats values <1 with a leading 0.
    printf '%d.%d\n' "${round1:0:-1}" "${round1:-1}"
}

COLUMNS=$(tput cols)

# Step names by session (: prefix)
declare -A steps
# Session names, by descending recency (surrounded by :)
all_sessions=:
# Job counts
num_dispatched=0
num_final=0
# Nanosecond-resolution monotonic timestamps
first_ns=
last_ns=

while read -r line; do
    # \e[K is ANSI Erase in Line (to right).
    echo -ne '\r\e[K'

    if [[ $line != !!* ]]; then
        echo "$line"
        continue
    elif [[ $line != !!step$'\t'* ]]; then
        continue
    fi
    IFS=$'\t' read -r _ ns session job state step <<< "$line"

    ## Update progress state

    if [[ -z $first_ns ]]; then
        first_ns=$ns
    fi
    last_ns=$ns

    if [[ $all_sessions != *:$session:* ]]; then
        if [[ $all_sessions == : ]]; then
            echo "Session $session"
        fi
        all_sessions=":$session$all_sessions" # newest sessions first
    fi

    if [[ $state == - ]]; then
        steps[$session]="${steps[$session]}:$step"
        ((num_dispatched++))
    else
        steps[$session]="${steps[$session]/:$step}"
        ((num_final++))
    fi

    ## Format status message

    msg="    job $num_final/$num_dispatched"

    IFS=:
    for session in ${all_sessions:1:-1}; do
        if [[ -n ${steps[$session]} ]]; then
            msg="$msg [${session:0:2}]${steps[$session]//:/ }"
        fi
    done
    IFS=

    if [[ ${#msg} -gt $COLUMNS ]]; then
        msg="${msg:0:$(( $COLUMNS - 3 ))}..."
    fi

    # Keep the cursor on this line so it will be replaced on the next iteration.
    echo -n "$msg"
done

if [[ $num_final -gt 0 ]]; then
    echo -ne '\r\e[K'
    plural=
    if [[ $num_final -gt 1 ]]; then
        plural=s
    fi
    echo "Ran $num_final uncached job$plural in" \
        "$(round1 $(( $last_ns - $first_ns )) 1000000000) seconds"
fi
