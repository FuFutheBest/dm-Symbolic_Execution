void check(int x) {
    int r = 0;
    if (x < -673) {
        r = -673;
    } else if (x > 65866) {
        r = 65866;
    } else {
        r = x;
    }
    assert(r >= -673 && r <= 65866);
}
