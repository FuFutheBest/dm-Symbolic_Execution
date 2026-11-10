void check(int x) {
    int s = x;
    if (x < 0) {
        s = -x;
    }
    assert(s >= 0);
}
