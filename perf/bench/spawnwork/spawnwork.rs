use std::thread;

fn steps(mut n: i64) -> i64 {
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
    let chunk: i64 = 625000;
    let mut handles = Vec::new();
    for w in 0..8i64 {
        let lo = if w == 0 { 1 } else { w * chunk };
        let hi = (w + 1) * chunk;
        handles.push(thread::spawn(move || {
            let mut t: i64 = 0;
            let mut i = lo;
            while i < hi {
                t += steps(i);
                i += 1;
            }
            t
        }));
    }
    let mut total: i64 = 0;
    for h in handles {
        total += h.join().unwrap();
    }
    println!("{}", total);
}
