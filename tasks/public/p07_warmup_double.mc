void check(int x) {
    int xx = x << 1;
    int overflow = (x >> 31) ^ (xx >> 31);
    xx = (xx & ~overflow) + ((overflow << 31) ^ (~(x >> 31) & overflow));
    assert(xx == x * 2);
}
