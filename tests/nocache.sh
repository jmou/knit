#!/bin/bash

. test-setup.sh

cat <<'EOF' > plan.knit
step random: nocache cmd "bash\0-c\0echo $RANDOM > out/result"
EOF

prd=$(expect_ok knit-run-plan)
prd2=$(expect_ok knit-run-plan)
expect_ok test "$prd" != "$prd2"
