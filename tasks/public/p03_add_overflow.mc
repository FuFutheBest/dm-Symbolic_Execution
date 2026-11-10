void check(int x, int y) {
    assume(x >= 0 && x <= 396339462);
    assume(y >= 0 && y <= 1751144185);
    int s = x + y;
    assert(s >= x && s >= y);
}
