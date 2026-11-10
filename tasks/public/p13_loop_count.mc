void check(int x) {
    assume(x >= 0 && x <= 9);
    int c = 0;
    for i in 0..9 {
        if (i <= x) {
            c = c + 1;
        }
    }
    assert(c <= 9);
}
