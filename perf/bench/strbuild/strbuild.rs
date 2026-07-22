fn main() {
    let mut total: i64 = 0;
    for _ in 0..1000000 {
        let mut s = String::new();
        for _ in 0..8 {
            // black_box: keep the allocation observable so -O2 can't delete it
            s = std::hint::black_box([s.as_str(), "abcdefg"].concat());
        }
        total += s.len() as i64;
    }
    println!("{}", total);
}
