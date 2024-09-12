#!/bin/bash

. test-setup.sh

cat <<'EOF' > plan.knit
step random: nocache cmd "bash\0-c\0echo $RANDOM > out/result"
EOF

inv=$(expect_ok knit-run-plan)
inv2=$(expect_ok knit-run-plan)
expect_ok test "$inv" != "$inv2"
