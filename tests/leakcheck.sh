#!/bin/sh
# Language Environment sees every heap request the program makes, so its
# storage report is a leak test: run each one-shot mode under RPTSTG(ON),
# RPTOPTS(ON) and HEAPCHK(ON), then read the report back.  A get without
# its free, or a created heap without its discard, is a leak.
#
#   tests/leakcheck.sh ./ztop          run the modes and check each report
#   tests/leakcheck.sh --report FILE   check one saved report
#   tests/leakcheck.sh --selftest      check the checker
set -eu

count() {
    sed -n "s/^$1: *//p" "$2" | {
        n=0
        while read -r v; do
            n=$((n + v))
        done
        echo "$n"
    }
}

report() {
    log=$1
    if ! grep -q 'Storage Report' "$log"; then
        echo "no storage report in $log"
        return 1
    fi
    g=$(count "Successful Get Heap requests" "$log")
    f=$(count "Successful Free Heap requests" "$log")
    c=$(count "Successful Create Heap requests" "$log")
    d=$(count "Successful Discard Heap requests" "$log")
    if [ "$g" -ne "$f" ] || [ "$c" -ne "$d" ]; then
        echo "LEAK: gets $g frees $f, create $c discard $d"
        return 1
    fi
    echo "heap gets $g frees $f, create $c discard $d"
}

if [ "${1:-}" = "--report" ]; then
    report "$2"
    exit
fi

dir=${HOME}/tmp/ztop/leak

if [ "${1:-}" = "--selftest" ]; then
    mkdir -p "$dir"
    printf 'STORAGE Report\nHEAP statistics:\nSuccessful Get Heap requests: 3\nSuccessful Free Heap requests: 3\nSuccessful Create Heap requests: 1\nSuccessful Discard Heap requests: 1\nEnd of Storage Report\n' > "$dir/clean.log"
    printf 'STORAGE Report\nHEAP statistics:\nSuccessful Get Heap requests: 5\nSuccessful Free Heap requests: 4\nSuccessful Create Heap requests: 0\nSuccessful Discard Heap requests: 0\nEnd of Storage Report\n' > "$dir/leak.log"
    report "$dir/clean.log"
    if report "$dir/leak.log"; then
        echo "selftest: a leak was not reported"
        exit 1
    fi
    echo "leakcheck: ok"
    exit
fi

bin=${1:-./ztop}
mkdir -p "$dir"
opts='RPTSTG(ON),RPTOPTS(ON),HEAPCHK(ON)'
run() {
    name=$1
    shift
    _CEE_RUNOPTS=$opts "$bin" "$@" > "$dir/$name.out" 2> "$dir/$name.log"
    printf '%-5s ' "$name"
    report "$dir/$name.log"
}

for a in csa sqa lsqa pvt; do
    run "$a" -a "$a"
done
run -s -s
run -j -j
run -p -p
