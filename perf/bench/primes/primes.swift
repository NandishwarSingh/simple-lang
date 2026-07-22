func isPrime(_ n: Int64) -> Bool {
    if n < 2 { return false }
    var d: Int64 = 2
    while d * d <= n {
        if n % d == 0 { return false }
        d += 1
    }
    return true
}
var count: Int64 = 0
var n: Int64 = 2
while n < 3000000 {
    if isPrime(n) { count += 1 }
    n += 1
}
print(count)
