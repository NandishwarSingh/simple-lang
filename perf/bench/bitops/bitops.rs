fn popcount(mut x: u64) -> i64 {
    let mut n: i64 = 0;
    while x != 0 {
        x = x & (x - 1);
        n += 1;
    }
    n
}

fn mix(mut h: u64) -> u64 {
    h = h ^ (h >> 33);
    h = h.wrapping_mul(18397679294719823053u64);
    h = h ^ (h >> 29);
    h
}

fn main() {
    let mut total: i64 = 0;
    let mut h: u64 = 12345;
    for _ in 0..20000000 {
        h = mix(h);
        total += popcount(h);
    }
    println!("{}", total);
}
