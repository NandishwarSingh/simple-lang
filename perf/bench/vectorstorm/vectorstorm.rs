const N: usize = 8192;

fn main() {
    let mut x = vec![0.0f64; N];
    let mut y = vec![0.0f64; N];
    for i in 0..N {
        x[i] = ((i * 7 % 1000) as f64) / 1000.0;
        y[i] = ((i * 13 % 1000) as f64) / 1000.0;
    }

    for _ in 0..120000 {
        for i in 0..N {
            x[i] = x[i] * 0.99 + y[i] * 0.01;
            y[i] = y[i] * 0.99 + x[i] * 0.01;
        }
    }

    for _ in 0..70000 {
        for i in 0..N {
            let v = x[i];
            x[i] = ((v * 0.5 + 0.25) * v + 0.1) * 0.5 + y[i] * 0.5;
        }
    }

    let mut a = vec![[0.0f64; 128]; 128];
    let mut b = vec![[0.0f64; 128]; 128];
    for i in 0..128 {
        for j in 0..128 {
            a[i][j] = (((i * 7 + j) % 100) as f64) / 100.0;
            b[i][j] = (((i * 5 + j * 3) % 100) as f64) / 100.0;
        }
    }
    let mut trace = 0.0f64;
    for _ in 0..200 {
        let mut c = vec![[0.0f64; 128]; 128];
        for i in 0..128 {
            for k in 0..128 {
                let aik = a[i][k];
                for j in 0..128 {
                    c[i][j] = c[i][j] + aik * b[k][j];
                }
            }
        }
        for i in 0..128 {
            trace = trace + c[i][i];
        }
    }

    let mut s = 0.0f64;
    for i in 0..N {
        s = s + x[i] + y[i];
    }
    s = s + trace;
    let q = (s * 1000000.0) as i64;
    println!("{}", q % 1000000000000);
}
