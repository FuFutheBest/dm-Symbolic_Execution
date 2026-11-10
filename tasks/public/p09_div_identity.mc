void check(int x, int y) {
    assume(y != 0);
    int q = x / y;
    int r = x % y;
    assert(q * y + r == x);
}
