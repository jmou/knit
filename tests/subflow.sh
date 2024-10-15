#!/bin/bash

. test-setup.sh

## Hello subflow

mkdir hello

cat <<\EOF > hello/plan.knit
step world: params
    subject = "World"

partial bash: cmd "/bin/bash\0-e\0in/script"
    script = !

step hello: partial bash
    script = ./hello.sh
    $subject = world:subject
EOF

cat <<\EOF > hello/hello.sh
echo "Hello, $subject!" > out/result
EOF

## Explicit context and plan

cat <<\EOF > plan.knit
# Errant reference to ./hello/plan.knit is just to flush out issues with more
# than 2 references to the same file.
step errant-reference: identity
    plan = ./hello/plan.knit

step subflow: flow ./hello/ ./hello/plan.knit
EOF

prd=$(expect_ok knit-run-plan)

diff - <(knit-cat-file -p $prd:result) <<\EOF
Hello, World!
EOF

## :plan shorthand

cat <<\EOF > plan.knit
step subflow: flow ./hello/ :plan.knit
    subject = "Pluto"
EOF

prd=$(expect_ok knit-run-plan)

diff - <(knit-cat-file -p $prd:result) <<\EOF
Hello, Pluto!
EOF

## Implicit plan.knit

cat <<\EOF > plan.knit
step subflow: flow ./hello/
    subject = "Pluto"
EOF

prd2=$(expect_ok knit-run-plan)
# The flow jobs will differ, but they should interpret to the same session (and
# thus invocation).
expect_ok test "$(knit-peel-spec $prd^{invocation})" == "$(knit-peel-spec $prd2^{invocation})"
