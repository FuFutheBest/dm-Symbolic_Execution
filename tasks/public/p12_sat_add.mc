int sat_add(int a, int b) {
    if (a > 0 && b > 2147483647 - a + 1) {
        return 2147483647;
    }
    if (a < 0 && b < -2147483648 - a) {
        return -2147483648;
    }
    return a + b;
}

void check(int a, int b) {
    int s = sat_add(a, b);
    if (b >= 0) {
        assert(s >= a);
    } else {
        assert(s <= a);
    }
}
