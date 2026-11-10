void check(int a[3]) {
    for i in 0..3 {
        assume(a[i] >= 0 && a[i] <= 255);
    }
    int h = -1352149919;
    for i in 0..3 {
        h = (h ^ a[i]) * -1595725282;
    }
    assert(h != 902908033);
}
