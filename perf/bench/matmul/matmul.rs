fn main() {
    let mut a = vec![[0i64; 200]; 200];
    let mut b = vec![[0i64; 200]; 200];
    let mut c = vec![[0i64; 200]; 200];
    for i in 0..200usize {
        for j in 0..200usize {
            a[i][j] = ((i + j) % 7) as i64;
            b[i][j] = ((i * j) % 5) as i64;
            c[i][j] = 0;
        }
    }
    for _rep in 0..20 {
    for i in 0..200usize {
        for k in 0..200usize {
            let aik = a[i][k];
            for j in 0..200usize {
                c[i][j] += aik * b[k][j];
            }
        }
    }
    }
    let mut sum: i64 = 0;
    for i in 0..200usize {
        sum += c[i][i];
    }
    println!("{}", sum);
}
