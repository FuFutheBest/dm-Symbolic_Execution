void check(int a[8]) {
    for i in 0..7 {
        int m = i;
        for j in 0..8 {
            if (j > i && a[j] < a[m]) {
                m = j;
            }
        }
        int t = a[i];
        a[i] = a[m];
        a[m] = t;
    }
    for i in 0..7 {
        assert(a[i] <= a[i + 1]);
    }
}
