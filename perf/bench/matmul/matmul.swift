var a = [[Int64]](repeating: [Int64](repeating: 0, count: 200), count: 200)
var b = a, c = a
for i in 0..<200 { for j in 0..<200 {
    a[i][j] = Int64((i + j) % 7); b[i][j] = Int64((i * j) % 5); c[i][j] = 0
} }
for _ in 0..<20 { for i in 0..<200 { for k in 0..<200 {
    let aik = a[i][k]
    for j in 0..<200 { c[i][j] &+= aik &* b[k][j] }
} } }
var sum: Int64 = 0
for i in 0..<200 { sum &+= c[i][i] }
print(sum)
