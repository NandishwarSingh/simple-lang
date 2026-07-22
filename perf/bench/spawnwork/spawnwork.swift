import Foundation

func steps(_ n0: Int64) -> Int64 {
    var n = n0, s: Int64 = 0
    while n != 1 { if n % 2 == 0 { n = n / 2 } else { n = 3 &* n &+ 1 }; s += 1 }
    return s
}
let chunk: Int64 = 625000
var totals = [Int64](repeating: 0, count: 8)
let lock = NSLock()
let group = DispatchGroup()
for w in 0..<8 {
    group.enter()
    Thread.detachNewThread {
        let lo = w == 0 ? Int64(1) : Int64(w) * chunk
        let hi = Int64(w + 1) * chunk
        var t: Int64 = 0
        var i = lo
        while i < hi { t += steps(i); i += 1 }
        lock.lock(); totals[w] = t; lock.unlock()
        group.leave()
    }
}
group.wait()
print(totals.reduce(0, &+))
