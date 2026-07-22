const N: usize = 2000000;

fn main() {
    let mut flags = vec![true; N];
    let mut count: i64 = 0;
    for _rep in 0..10 {
    count = 0;
    for i in 0..N { flags[i] = true; }
    for i in 2..N {
        if flags[i] {
            count += 1;
            let mut j = i + i;
            while j < N {
                flags[j] = false;
                j += i;
            }
        }
    }
    }
    println!("{}", count);
}
