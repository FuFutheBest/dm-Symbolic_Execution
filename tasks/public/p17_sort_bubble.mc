void check(int a[5]) {
    for i in 0..4 {
        for j in 0..4 {
            if (j >= 4 - i) { break; }
            if (a[j] > a[j + 1]) {
                int t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
        }
    }
    for i in 0..4 {
        assert(a[i] <= a[i + 1]);
    }
}
