fn rng_step(s: u64) -> u64 {
    s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407)
}
fn rng_val(s: u64) -> i64 { (s >> 33) as i64 }

#[derive(Clone, Copy)]
struct Body { x: i64, y: i64, z: i64, vx: i64, vy: i64, vz: i64 }

fn interact(a: Body, b: Body) -> Body {
    let dx = b.x - a.x; let dy = b.y - a.y; let dz = b.z - a.z;
    let d2 = dx*dx + dy*dy + dz*dz + 1;
    let inv = 1000000 / (d2/1000 + 1);
    Body { x: a.x, y: a.y, z: a.z,
        vx: a.vx + (dx*inv)/100000,
        vy: a.vy + (dy*inv)/100000,
        vz: a.vz + (dz*inv)/100000 }
}

fn phase_a() -> i64 {
    let mut bs = [Body { x:0,y:0,z:0,vx:0,vy:0,vz:0 }; 32];
    let mut r: u64 = 88172645463325252;
    for i in 0..32 {
        r = rng_step(r); let x = rng_val(r) % 1000;
        r = rng_step(r); let y = rng_val(r) % 1000;
        r = rng_step(r); let z = rng_val(r) % 1000;
        bs[i] = Body { x, y, z, vx:0, vy:0, vz:0 };
    }
    for _ in 0..30000 {
        for i in 0..32 {
            for j in 0..32 {
                if i != j { bs[i] = interact(bs[i], bs[j]); }
            }
        }
    }
    let mut sum = 0i64;
    for i in 0..32 { sum += bs[i].vx + bs[i].vy + bs[i].vz; }
    sum
}

fn phase_b() -> u64 {
    let mut items: Vec<String> = Vec::new();
    for k in 0..500000i64 {
        items.push(format!("k{}_{}", k, (k*13) % 97));
    }
    let mut acc: u64 = 0;
    for s in &items {
        let mut h: u64 = 14695981039346656037;
        for &c in s.as_bytes() {
            h = (h ^ c as u64).wrapping_mul(1099511628211);
        }
        acc ^= h;
    }
    acc + items.len() as u64
}

fn phase_c() -> i64 {
    let mut a = [0i64; 25000];
    let mut r: u64 = 99887766554433;
    for i in 0..25000 { r = rng_step(r); a[i] = rng_val(r) % 1000000; }
    for i in 1..25000 {
        let v = a[i]; let mut j = i as i64 - 1;
        while j >= 0 && a[j as usize] > v { a[(j+1) as usize] = a[j as usize]; j -= 1; }
        a[(j+1) as usize] = v;
    }
    let mut s = 0i64;
    for i in 0..25000 { s += a[i] * i as i64; }
    s
}

fn fib(n: i64) -> i64 { if n < 2 { n } else { fib(n-1) + fib(n-2) } }
fn phase_d() -> i64 { fib(40) }

fn phase_e() -> i64 {
    let mut a = [[0i64; 100]; 100];
    let mut b = [[0i64; 100]; 100];
    for i in 0..100 {
        for j in 0..100 {
            a[i][j] = ((i*7 + j) % 13) as i64;
            b[i][j] = ((i*5 + j*3) % 11) as i64;
        }
    }
    let mut acc = 0i64;
    for _ in 0..150 {
        let mut c = [[0i64; 100]; 100];
        for i in 0..100 {
            for k in 0..100 {
                let aik = a[i][k];
                for j in 0..100 { c[i][j] += aik * b[k][j]; }
            }
        }
        for i in 0..100 { acc += c[i][i]; }
    }
    acc
}

fn main() {
    let a = phase_a();
    let b = phase_b();
    let c = phase_c();
    let d = phase_d();
    let e = phase_e();
    let mut sum: u64 = 0;
    sum = sum.wrapping_add(a as u64);
    sum ^= b;
    sum = sum.wrapping_add(c as u64);
    sum ^= d as u64;
    sum = sum.wrapping_add(e as u64);
    println!("{}", sum % 1000000000000);
}
