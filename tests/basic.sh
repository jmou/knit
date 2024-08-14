#!/bin/bash

. test-setup.sh

cat <<'EOF' > plan.knit
step a: shell
    shell = "seq 1 3 > out/data"

step b: shell
    shell = "cp -RL in/* out"
    a.ok = a:.knit/ok
    data = a:data

step c: shell
    shell = "tac in/lines > out/data"
    b.ok = b:.knit/ok
    lines = b:data
EOF

expect_ok knit-run-plan
expect_ok knit-run-plan  # should hit cache
