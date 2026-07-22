fn main() {
    let mut a = [0i64; 800];
    let mut check: i64 = 0;
    let mut seed: u64 = 987654321;
    for _ in 0..600 {
        for i in 0..800usize {
            seed = seed
                .wrapping_mul(6364136223846793005u64)
                .wrapping_add(1442695040888963407u64);
            a[i] = (seed >> 40) as i64;
        }
        for i in 1..800usize {
            let v = a[i];
            let mut j = i as i64 - 1;
            while j >= 0 && a[j as usize] > v {
                a[(j + 1) as usize] = a[j as usize];
                j -= 1;
            }
            a[(j + 1) as usize] = v;
        }
        check += a[0] + a[799];
    }
    println!("{}", check);
}
