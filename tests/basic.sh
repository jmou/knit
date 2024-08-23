#!/bin/bash

. test-setup.sh

cat <<'EOF' > plan.knit
partial bash: cmd "bash\0in/script"
    script = !

step params: params
    limit = "3"

step a: partial bash
    script = "seq 1 $(< in/limit) > out/data"
    limit = params:limit

step b: partial bash
    script = "cp -RL in/* out"
    a.ok = a:.knit/ok
    data ?= a:data
    # This comment will be ignored. Empty lines are ok.

    optional ?= a:nonexistent

step c: identity
    b.ok = b:.knit/ok
    renamed = b:data

step asomewhatlongstepname: partial bash
    script = "tac in/lines > out/data"
    c.ok = c:.knit/ok
    lines = c:renamed
EOF

inv=$(expect_ok knit-run-plan)
expect_ok test $(knit-run-plan -v -p limit=3 2>&1 | grep 'Cache hit' | wc -l) -eq 5
inv2=$(expect_ok knit-run-plan -p limit=3)
expect_ok test "$inv" == "$inv2"
expect_ok knit-run-plan -p limit=5

cat <<'EOF' > super.knit
step sub: flow ./plan.knit
    params/limit = "5"
EOF

inv=$(expect_ok knit-run-plan -f super.knit)
inv2=$(expect_ok knit-run-plan -p limit=5)
expect_ok test "$(knit-peel-spec $inv^{production}^{invocation})" == "$inv2"
