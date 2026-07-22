var a = [Int64](repeating: 0, count: 800)
var check: Int64 = 0
var seed: UInt64 = 987654321
for _ in 0..<600 {
    for i in 0..<800 {
        seed = seed &* 6364136223846793005 &+ 1442695040888963407
        a[i] = Int64(seed >> 40)
    }
    for i in 1..<800 {
        let v = a[i]
        var j = i - 1
        while j >= 0 && a[j] > v { a[j + 1] = a[j]; j -= 1 }
        a[j + 1] = v
    }
    check &+= a[0] &+ a[799]
}
print(check)
