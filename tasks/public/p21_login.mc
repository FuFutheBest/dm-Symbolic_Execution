void check(int m[12]) {
    for i in 0..12 {
        assume(m[i] >= 0 && m[i] <= 255);
    }
    int state = 0;
    int fails = 0;
    int acc = 0;
    for i in 0..12 {
        int c = m[i];
        if (state == 0) {
            if (c == 'U') {
                state = 1;
                acc = -1814952848;
            }
        } else if (state == 1) {
            if (c == ';') {
                if (acc == 756756682) {
                    state = 2;
                } else {
                    fails = fails + 1;
                    state = 0;
                }
            } else {
                acc = (acc ^ c) * 0x01000193;
            }
        } else if (state == 2) {
            if (c == 'A' && fails >= 3) {
                state = 3;
            } else if (c == 'Q') {
                state = 0;
            }
        }
    }
    assert(state != 3);
}
