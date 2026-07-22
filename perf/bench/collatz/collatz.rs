fn steps(n0: i64) -> i64 {
    let mut n = n0;
    let mut s: i64 = 0;
    while n != 1 {
        if n % 2 == 0 {
            n = n / 2;
        } else {
            n = 3 * n + 1;
        }
        s += 1;
    }
    s
}

fn main() {
    let mut total: i64 = 0;
    let mut i: i64 = 1;
    while i < 5000000 {
        total += steps(i);
        i += 1;
    }
    println!("{}", total);
}
