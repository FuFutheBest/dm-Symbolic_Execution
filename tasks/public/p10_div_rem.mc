void check(int x) {
    int q = x / 100;
    int r = x % 100;
    assert(q * 100 + r == x);
    assert(r > -100 && r < 100);
}
