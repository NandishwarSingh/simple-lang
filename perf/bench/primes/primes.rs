fn is_prime(n: i64) -> bool {
    if n < 2 {
        return false;
    }
    let mut d: i64 = 2;
    while d * d <= n {
        if n % d == 0 {
            return false;
        }
        d += 1;
    }
    true
}

fn main() {
    let mut count: i64 = 0;
    let mut n: i64 = 2;
    while n < 3000000 {
        if is_prime(n) {
            count += 1;
        }
        n += 1;
    }
    println!("{}", count);
}
