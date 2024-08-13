#!/bin/bash

. test-setup.sh

cat <<'EOF' > plan.knit
partial bash: shell
    shell = "bash in/script"
    script = !

step a: partial bash
    script = "seq 1 3 > out/data"

step b: partial bash
    script = "cp -RL in/* out"
    a.ok = a:.knit/ok
    data = a:data

step c: partial bash
    script = "tac in/lines > out/data"
    b.ok = b:.knit/ok
    lines = b:data
EOF

expect_ok knit-run-plan
expect_ok knit-run-plan  # should hit cache
