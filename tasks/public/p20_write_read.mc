void check(int i, int j, int v) {
    assume(i >= 0 && i < 8 && j >= 0 && j < 8);
    int a[8];
    for t in 0..8 {
        a[t] = 1461983573;
    }
    a[i] = v;
    if (i != j) {
        assert(a[j] != v);
    }
}
