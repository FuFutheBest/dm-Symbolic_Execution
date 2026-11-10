void check(int n, int x) {
    assume(n >= 0 && n <= 9);
    int s = 0;
    for i in 0..8 {
        if (i >= n) {
            break;
        }
        s = s + x;
    }
    assert(s == n * x);
}
