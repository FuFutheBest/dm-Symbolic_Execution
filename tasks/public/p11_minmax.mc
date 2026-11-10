int bigger(int a, int b) {
    if (a >= b) {
        return a;
    }
    return b;
}

int smaller(int a, int b) {
    if (a <= b) {
        return a;
    }
    return b;
}

void check(int a, int b) {
    assert(bigger(a, b) + smaller(a, b) == a + b);
    assert(bigger(a, b) >= smaller(a, b));
}
