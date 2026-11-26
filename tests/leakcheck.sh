#!/bin/sh
# Language Environment sees every heap request the program makes, so its
# storage report can be read as a leak test: run each one-shot mode under
# RPTSTG(ON), RPTOPTS(ON) and HEAPCHK(ON), then compare what the heap
# looked like at the end of each run.  ztop allocates nothing itself, so
# every mode has to end with the same heap.  A mode that ends with more
# used storage than the others is holding something it should have freed.
#
#   tests/leakcheck.sh ./ztop          run the modes and compare them
#   tests/leakcheck.sh --report FILE   describe one saved report
#   tests/leakcheck.sh --selftest      check the checker
set -eu

count() {
    sed -n "s/^ *$1: *//p" "$2" | {
        n=0
        while read -r v; do
            n=$((n + v))
        done
        echo "$n"
    }
}

stats() {
    log=$1
    if ! grep -q 'HEAP statistics:' "$log"; then
        echo "no heap report in $log" >&2
        return 1
    fi
    g=$(count "Successful Get Heap requests" "$log")
    f=$(count "Successful Free Heap requests" "$log")
    u=$(count "Total heap storage used (sugg. initial size)" "$log")
    echo "gets $g frees $f used $u"
}

if [ "${1:-}" = "--report" ]; then
    stats "$2"
    exit
fi

dir=${HOME}/tmp/ztop/leak

if [ "${1:-}" = "--selftest" ]; then
    mkdir -p "$dir"
    printf 'HEAP statistics:\n      Total heap storage used (sugg. initial size): 3328\n      Successful Get Heap requests: 4\n      Successful Free Heap requests: 0\n' > "$dir/one.log"
    printf 'HEAP statistics:\n      Total heap storage used (sugg. initial size): 3328\n      Successful Get Heap requests: 4\n      Successful Free Heap requests: 0\n' > "$dir/two.log"
    printf 'HEAP statistics:\n      Total heap storage used (sugg. initial size): 4096\n      Successful Get Heap requests: 5\n      Successful Free Heap requests: 0\n' > "$dir/leak.log"
    one=$(stats "$dir/one.log")
    two=$(stats "$dir/two.log")
    if [ "$one" != "$two" ]; then
        echo "selftest: identical reports compared unequal"
        exit 1
    fi
    if [ "$one" = "$(stats "$dir/leak.log")" ]; then
        echo "selftest: a bigger heap was not noticed"
        exit 1
    fi
    echo "leakcheck: ok"
    exit
fi

bin=${1:-./ztop}
mkdir -p "$dir"
opts='RPTSTG(ON),RPTOPTS(ON),HEAPCHK(ON)'
base=''
run() {
    name=$1
    shift
    _CEE_RUNOPTS=$opts "$bin" "$@" > "$dir/$name.out" 2> "$dir/$name.log"
    s=$(stats "$dir/$name.log")
    if [ -z "$base" ]; then
        base=$s
        printf '%-5s %s\n' "$name" "$s"
        return
    fi
    if [ "$s" != "$base" ]; then
        printf 'LEAK  %-5s %s (first run: %s)\n' "$name" "$s" "$base"
        return 1
    fi
    printf '%-5s %s\n' "$name" "$s"
}

for a in csa sqa lsqa pvt; do
    run "$a" -a "$a"
done
run -s -s
run -j -j
run -p -p
