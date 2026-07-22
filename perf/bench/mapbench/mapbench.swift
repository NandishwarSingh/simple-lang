var a = [Int64: Int64]()
for i in 0..<10000000 as Range<Int64> {
    a[(i * 2654435761) % 4000037] = i
}
var suma: Int64 = 0
for i in 0..<10000000 as Range<Int64> {
    let k = (i * 7919) % 4000037
    if let v = a[k] { suma += v }
}
for i in 0..<1000000 as Range<Int64> {
    a.removeValue(forKey: (i * 31) % 4000037)
}
suma += Int64(a.count) * 17

var wc = [String: Int64]()
for i in 0..<1500000 as Range<Int64> {
    let w = "w\((i * 131) % 9973)"
    wc[w, default: 0] += 1
}
var sumb: Int64 = 0
for i in 0..<9973 as Range<Int64> {
    if let v = wc["w\(i)"] { sumb += v * (i + 1) }
}
var sumc: Int64 = 0
for (_, v) in wc { sumc += v }

print(suma + sumb * 3 + sumc)
