# Read the storage report Language Environment writes at termination
# (RPTSTG(ON)) and fail when the heap was not given back: a get without
# its free, or a created heap without its discard, is a leak.
#
#   awk -f tests/leak.awk < storage-report

/^(HEAP|HEAP24|ANYHEAP|BELOWHEAP|THREADHEAP|Additional Heap) statistics:/ {
    inheap = 1
}

/^End of Storage Report/ {
    inheap = 0
}

inheap && /^Successful Get Heap requests:/ {
    get += $NF
}

inheap && /^Successful Free Heap requests:/ {
    fre += $NF
}

inheap && /^Successful Create Heap requests:/ {
    cre += $NF
}

inheap && /^Successful Discard Heap requests:/ {
    dis += $NF
}

END {
    if (get != fre || cre != dis) {
        printf "LEAK: gets %d frees %d, create %d discard %d\n", get, fre,
               cre, dis
        exit 1
    }
    printf "heap gets %d frees %d, create %d discard %d\n", get, fre, cre,
           dis
}
