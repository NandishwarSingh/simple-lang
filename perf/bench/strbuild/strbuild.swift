var total: Int64 = 0
for _ in 0..<1000000 {
    var s = ""
    for _ in 0..<8 { s = s + "abcdefg" }
    total += Int64(s.utf8.count)
}
print(total)
