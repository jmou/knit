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

step d: partial bash
    script = "split -l1 in/lines out/"
    c.ok = c:.knit/ok
    lines = c:renamed

step e: identity
    d.ok = d:.knit/ok
    1/ = d:
    2/ = d:

step f: partial bash
    e.ok = e:.knit/ok
    script = "rm in/e2/aa && cat in/e1/aa in/e2/* > out/combined"
    e1/ = e:1/
    e2/ = e:2/

step asomewhatlongstepname: partial bash
    script = "tac in/lines > out/data"
    f.ok = f:.knit/ok
    lines = f:combined
EOF

inv=$(expect_ok knit-run-plan)
expect_ok test $(knit-run-plan -v -p limit=3 2>&1 | grep 'Cache hit' | wc -l) -eq 8
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
