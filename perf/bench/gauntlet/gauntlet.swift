func rngStep(_ s: UInt64) -> UInt64 {
    return s &* 6364136223846793005 &+ 1442695040888963407
}
func rngVal(_ s: UInt64) -> Int64 { return Int64(bitPattern: s >> 33) }

struct Body { var x: Int64; var y: Int64; var z: Int64; var vx: Int64; var vy: Int64; var vz: Int64 }

func interact(_ a: Body, _ b: Body) -> Body {
    let dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z
    let d2 = dx*dx + dy*dy + dz*dz + 1
    let inv = 1000000 / (d2/1000 + 1)
    return Body(x: a.x, y: a.y, z: a.z,
                vx: a.vx + (dx*inv)/100000,
                vy: a.vy + (dy*inv)/100000,
                vz: a.vz + (dz*inv)/100000)
}

func phaseA() -> Int64 {
    var bs = [Body](repeating: Body(x:0,y:0,z:0,vx:0,vy:0,vz:0), count: 32)
    var r: UInt64 = 88172645463325252
    for i in 0..<32 {
        r = rngStep(r); let x = rngVal(r) % 1000
        r = rngStep(r); let y = rngVal(r) % 1000
        r = rngStep(r); let z = rngVal(r) % 1000
        bs[i] = Body(x: x, y: y, z: z, vx: 0, vy: 0, vz: 0)
    }
    for _ in 0..<30000 {
        for i in 0..<32 {
            for j in 0..<32 where i != j {
                bs[i] = interact(bs[i], bs[j])
            }
        }
    }
    var sum: Int64 = 0
    for i in 0..<32 { sum += bs[i].vx + bs[i].vy + bs[i].vz }
    return sum
}

func phaseB() -> UInt64 {
    var items = [String]()
    items.reserveCapacity(500000)
    for k in 0..<500000 {
        items.append("k\(k)_\((k*13) % 97)")
    }
    var acc: UInt64 = 0
    for s in items {
        var h: UInt64 = 14695981039346656037
        for c in s.utf8 {
            h = (h ^ UInt64(c)) &* 1099511628211
        }
        acc ^= h
    }
    return acc + UInt64(items.count)
}

func phaseC() -> Int64 {
    var a = [Int64](repeating: 0, count: 25000)
    var r: UInt64 = 99887766554433
    for i in 0..<25000 { r = rngStep(r); a[i] = rngVal(r) % 1000000 }
    for i in 1..<25000 {
        let v = a[i]; var j = i - 1
        while j >= 0 && a[j] > v { a[j+1] = a[j]; j -= 1 }
        a[j+1] = v
    }
    var s: Int64 = 0
    for i in 0..<25000 { s += a[i] * Int64(i) }
    return s
}

func fib(_ n: Int64) -> Int64 { return n < 2 ? n : fib(n-1) + fib(n-2) }
func phaseD() -> Int64 { return fib(40) }

func phaseE() -> Int64 {
    var a = [[Int64]](repeating: [Int64](repeating: 0, count: 100), count: 100)
    var b = a
    for i in 0..<100 {
        for j in 0..<100 {
            a[i][j] = Int64((i*7 + j) % 13)
            b[i][j] = Int64((i*5 + j*3) % 11)
        }
    }
    var acc: Int64 = 0
    for _ in 0..<150 {
        var c = [[Int64]](repeating: [Int64](repeating: 0, count: 100), count: 100)
        for i in 0..<100 {
            for k in 0..<100 {
                let aik = a[i][k]
                for j in 0..<100 { c[i][j] += aik * b[k][j] }
            }
        }
        for i in 0..<100 { acc += c[i][i] }
    }
    return acc
}

let a = phaseA()
let b = phaseB()
let c = phaseC()
let d = phaseD()
let e = phaseE()
var sum: UInt64 = 0
sum = sum &+ UInt64(bitPattern: a)
sum ^= b
sum = sum &+ UInt64(bitPattern: c)
sum ^= UInt64(bitPattern: d)
sum = sum &+ UInt64(bitPattern: e)
print(sum % 1000000000000)
