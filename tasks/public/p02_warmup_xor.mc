void check(int x, int y) {
    int a = ~((~(~x & y)) & (~(x & ~y)));
    assert(a != (x ^ y));
}
