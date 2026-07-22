import Foundation

final class Slot {
    private let lock = NSCondition()
    private var v: Int64 = 0
    private var full = false
    func send(_ val: Int64) {
        lock.lock(); while full { lock.wait() }
        v = val; full = true; lock.signal(); lock.unlock()
    }
    func recv() -> Int64 {
        lock.lock(); while !full { lock.wait() }
        let r = v; full = false; lock.signal(); lock.unlock()
        return r
    }
}
let ping = Slot(), pong = Slot()
let t = Thread {
    while true {
        let v = ping.recv()
        if v == -1 { return }
        pong.send(v + 1)
    }
}
t.start()
var total: Int64 = 0
for i in 0..<100000 { ping.send(Int64(i)); total += pong.recv() }
ping.send(-1)
print(total)
