#!/bin/bash

. test-setup.sh

cat <<'EOF' > plan.knit
step a: external
    x = "1"

step b: identity
    y = a:y
EOF

expect_ok knit-run-plan 2>&1 > prd | while read -r stderr; do
    if [[ $stderr != "External job "* ]]; then
        echo "$stderr" >&2
        continue
    fi
    job="${stderr#External job }"
    expect_ok test "$(knit-cat-file resource $job:x)" == 1
    echo 2 > y
    knit-complete-job --job $job --file y
done

expect_ok test "$(knit-cat-file -p $(< prd):y)" == 2
