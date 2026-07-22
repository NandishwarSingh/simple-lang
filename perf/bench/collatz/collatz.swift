func steps(_ n0: Int64) -> Int64 {
    var n = n0
    var s: Int64 = 0
    while n != 1 {
        if n % 2 == 0 { n = n / 2 } else { n = 3 &* n &+ 1 }
        s += 1
    }
    return s
}
var total: Int64 = 0
var i: Int64 = 1
while i < 5000000 {
    total += steps(i)
    i += 1
}
print(total)
