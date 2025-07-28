#!/bin/bash

. test-setup.sh

# TODO production with invocation

cat > instructions <<\EOF
resource 0
entry .knit/identity
resource 8
input 1
entry name1
resource 8
input 2
entry name2
link 2
job
resource 7
output
entry name
production
pop production
EOF
oid=$(expect_ok knit-build-objects < instructions)
expect_ok test $oid == 9c16aa143ca4cb2c5b32d4fa1c43fccf554d662b43accd5f99ed9f7b800ef8f2

# out of order resource names are invalid
cat > instructions <<\EOF
resource 0
entry .knit/identity
resource 0
entry b
resource 0
entry a
link 2
job
pop job
EOF
expect_fail knit-build-objects < instructions

# NUL disallowed in resource name
{
    echo resource 0
    echo entry .knit/identity
    echo resource 0
    printf "entry nul\0name\n"
    echo link 1
    echo job
    echo pop job
} > instructions
expect_fail knit-build-objects < instructions

# NUL allowed in resource data
{
    echo resource 0
    echo entry .knit/identity
    echo resource 8
    printf "nul\0data"
    echo entry isok
    echo link 1
    echo job
    echo pop job
} > instructions
oid=$(expect_ok knit-build-objects < instructions)
expect_ok test $oid == aba2c9650576a3143cbcfb6d16af22194d873d5d316853b8d88c38448e811888
