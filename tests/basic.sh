#!/bin/bash

. test-setup.sh

echo -n 1 > start
echo -n 3 > limit
mkdir empty

cat <<'EOF' > plan.knit
partial bash: cmd "bash\0in/script"
    script = !

step params: params
    start = ./start
    limit = ./limit

step a: partial bash
    script = "seq $(< in/start) $limit > out/data"
    start = params:start
    $limit = params:limit

step b: partial bash
    script = "cp -RL in/* out"
    data ?= a:data
    # This comment will be ignored. Empty lines are ok.

    optional ?= a:nonexistent
    optional2/ ?= ./empty/

step c: identity
    renamed = b:data

step d: partial bash
    script = "split -l1 in/lines out/ && false"
    lines = c:renamed

step e: identity
    1/ ?:= d:
    2/ := d:

step f: partial bash
    script = "rm in/e2/aa && cat in/e1/aa in/e2/* > out/combined"
    e1/ = e:1/
    e2/ = e:2/

step asomewhatlongstepname: partial bash
    script = "tac in/lines > out/data"
    lines = f:combined
EOF

prd=$(expect_ok knit-run-plan)
expect_ok test $(knit-run-plan --no-filter -p limit=3 2>&1 | grep ^!!cache-hit | wc -l) -eq 8
prd2=$(expect_ok knit-run-plan -p limit=3)
expect_ok test "$(knit-peel-spec $prd^{invocation})" == "$(knit-peel-spec $prd2^{invocation})"
expect_ok knit-run-plan -p limit=5

diff - <(knit-cat-file -p @:data) <<'EOF'
5
4
3
2
1
EOF

cat <<'EOF' > super.knit
step sub: flow ./ :plan.knit
    limit = "5"
EOF

prd=$(expect_ok knit-run-plan -f super.knit)
prd2=$(expect_ok knit-run-plan -p limit=5)
expect_ok test "$(knit-peel-spec $prd^{invocation}^{production}^{invocation})" == "$(knit-peel-spec $prd2^{invocation})"
