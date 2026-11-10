void check(int x) {
    int y = x;
    for i in 0..6 {
        y = y * 2108418345 + -1775879450;
    }
    assert(y != 678541927);
}
