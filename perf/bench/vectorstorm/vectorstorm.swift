let n = 8192

var x = [Double](repeating: 0.0, count: n)
var y = [Double](repeating: 0.0, count: n)
for i in 0..<n {
    x[i] = Double((i * 7) % 1000) / 1000.0
    y[i] = Double((i * 13) % 1000) / 1000.0
}

for _ in 0..<120000 {
    for i in 0..<n {
        x[i] = x[i] * 0.99 + y[i] * 0.01
        y[i] = y[i] * 0.99 + x[i] * 0.01
    }
}

for _ in 0..<70000 {
    for i in 0..<n {
        let v = x[i]
        x[i] = ((v * 0.5 + 0.25) * v + 0.1) * 0.5 + y[i] * 0.5
    }
}

var a = [[Double]](repeating: [Double](repeating: 0.0, count: 128), count: 128)
var b = a
for i in 0..<128 {
    for j in 0..<128 {
        a[i][j] = Double((i * 7 + j) % 100) / 100.0
        b[i][j] = Double((i * 5 + j * 3) % 100) / 100.0
    }
}
var trace = 0.0
for _ in 0..<200 {
    var c = [[Double]](repeating: [Double](repeating: 0.0, count: 128), count: 128)
    for i in 0..<128 {
        for k in 0..<128 {
            let aik = a[i][k]
            for j in 0..<128 {
                c[i][j] = c[i][j] + aik * b[k][j]
            }
        }
    }
    for i in 0..<128 {
        trace = trace + c[i][i]
    }
}

var s = 0.0
for i in 0..<n {
    s = s + x[i] + y[i]
}
s = s + trace
let q = Int64(s * 1000000.0)
print(q % 1000000000000)
