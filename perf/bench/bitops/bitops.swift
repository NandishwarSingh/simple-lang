func popcount(_ x0: UInt64) -> Int64 {
    var x = x0
    var n: Int64 = 0
    while x != 0 { x = x & (x - 1); n += 1 }
    return n
}
func mix(_ h0: UInt64) -> UInt64 {
    var h = h0
    h = h ^ (h >> 33)
    h = h &* 18397679294719823053
    h = h ^ (h >> 29)
    return h
}
var total: Int64 = 0
var h: UInt64 = 12345
for _ in 0..<20000000 { h = mix(h); total += popcount(h) }
print(total)
