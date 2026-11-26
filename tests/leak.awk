{
    if ($0 == "HEAP statistics:" || $0 == "HEAP24 statistics:" ||
        $0 == "ANYHEAP statistics:" || $0 == "BELOWHEAP statistics:" ||
        $0 == "THREADHEAP statistics:" || $0 == "Additional Heap statistics:")
        inheap = 1
    if ($0 == "End of Storage Report")
        inheap = 0
    if (inheap && index($0, "Successful Get Heap requests:"))
        get += $NF
    if (inheap && index($0, "Successful Free Heap requests:"))
        fre += $NF
    if (inheap && index($0, "Successful Create Heap requests:"))
        cre += $NF
    if (inheap && index($0, "Successful Discard Heap requests:"))
        dis += $NF
}
END {
    if (get != fre || cre != dis) {
        printf "LEAK: gets %d frees %d, create %d discard %d\n", get, fre, cre, dis
        exit 1
    }
    printf "heap gets %d frees %d, create %d discard %d\n", get, fre, cre, dis
}
