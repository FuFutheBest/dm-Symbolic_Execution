void check(int x) {
    int r = 0;
    if (x < -36548) {
        r = 0;
    } else if (x < -20734) {
        r = 1;
    } else if (x <= 34829) {
        r = 2;
    } else {
        r = 3;
    }
    if (r == 0) {
        assert(x < -36548);
    } else if (r == 1) {
        assert(x >= -36548 && x < -20734);
    } else if (r == 2) {
        assert(x >= -20734 && x < 34829);
    } else {
        assert(x >= 34829);
    }
}
