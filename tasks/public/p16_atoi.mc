void check(int s[12]) {
    int r = 0;
    int start = 0;
    bool neg = false;
    int digits = 0;
    if (s[0] == '-') {
        neg = true;
        start = 1;
    }
    for i in 0..12 {
        if (i < start) { continue; }
        int c = s[i];
        if (c == 0) { break; }
        if (c < '0' || c > '9') { return; }
        int d = c - '0';
        if (r > 214748364 || (r == 214748364 && d > 8)) { return; }
        r = r * 10 + d;
        digits = digits + 1;
    }
    if (digits == 0) { return; }
    if (neg) {
        r = -r;
        assert(r <= 0);
    } else {
        assert(r >= 0);
    }
}
