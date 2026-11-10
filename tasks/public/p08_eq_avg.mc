void check(int a, int b) {
    int x = (a & b) + ((a ^ b) >> 1);
    int y = (a >> 1) + (b >> 1) + (a & b & 1);
    assert(x == y);
}
