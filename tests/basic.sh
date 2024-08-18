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
    data ?= a:data
    # This comment will be ignored. Empty lines are ok.

    optional ?= a:nonexistent

step asomewhatlongstepname: partial bash
    script = "tac in/lines > out/data"
    b.ok = b:.knit/ok
    lines = b:data
EOF

inv=$(expect_ok knit-run-plan)
knit-run-plan -v 2>&1 | grep -q 'Cache hit'
inv2=$(expect_ok knit-run-plan)
expect_ok test "$inv" == "$inv2"
