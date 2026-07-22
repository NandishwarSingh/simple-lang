let N = 2000000
var flags = [Bool](repeating: true, count: N)
var count: Int64 = 0
for _ in 0..<10 {
    count = 0
    for i in 0..<N { flags[i] = true }
    var i = 2
    while i < N {
        if flags[i] {
            count += 1
            var j = i + i
            while j < N { flags[j] = false; j += i }
        }
        i += 1
    }
}
print(count)
