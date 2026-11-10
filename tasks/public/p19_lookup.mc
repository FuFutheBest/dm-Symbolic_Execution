void check(int key) {
    int t[16];
    for i in 0..16 {
        t[i] = i * 25 + -9;
    }
    assume(key >= 13508938 && key <= 13508953);
    int i = key - 13508938;
    assert(t[i] == i * 25 + -9);
}
