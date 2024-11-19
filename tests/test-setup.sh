set -e

PATH="$PWD/..:$PATH"

die() {
    echo FAIL: "$@" >&2
    exit 1
}

expect_ok() {
    "$@" || die expect_ok "$@"
}

rm -rf tmp
knit init tmp 2> /dev/null || die 'has knit been built?'
cd tmp
