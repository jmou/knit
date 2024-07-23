#!/bin/bash -e

[[ $# -eq 2 ]]
job_id="$1"
outputs="$2"

# TODO production directory
mkdir -p .knit/stub-productions

production=$(mktemp -u .knit/stub-productions/XXXXXX)
cp -R "$outputs" $production
echo $production
