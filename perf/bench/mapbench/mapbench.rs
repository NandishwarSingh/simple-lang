use std::collections::HashMap;

fn main() {
    let mut a: HashMap<i64, i64> = HashMap::new();
    for i in 0..10000000i64 {
        a.insert((i * 2654435761) % 4000037, i);
    }
    let mut suma: i64 = 0;
    for i in 0..10000000i64 {
        let k = (i * 7919) % 4000037;
        if let Some(v) = a.get(&k) {
            suma += v;
        }
    }
    for i in 0..1000000i64 {
        a.remove(&((i * 31) % 4000037));
    }
    suma += a.len() as i64 * 17;

    let mut wc: HashMap<String, i64> = HashMap::new();
    for i in 0..1500000i64 {
        let w = format!("w{}", (i * 131) % 9973);
        *wc.entry(w).or_insert(0) += 1;
    }
    let mut sumb: i64 = 0;
    for i in 0..9973i64 {
        let w = format!("w{}", i);
        if let Some(v) = wc.get(&w) {
            sumb += v * (i + 1);
        }
    }
    let sumc: i64 = wc.values().sum();

    println!("{}", suma + sumb * 3 + sumc);
}
