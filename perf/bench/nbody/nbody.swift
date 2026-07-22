struct P { var x: Int64; var y: Int64; var vx: Int64; var vy: Int64 }
func interact(_ a: P, _ b: P) -> P {
    let dx = b.x &- a.x
    let dy = b.y &- a.y
    return P(x: a.x, y: a.y, vx: a.vx &+ dx / 16, vy: a.vy &+ dy / 16)
}
var ps = [P](repeating: P(x: 0, y: 0, vx: 1, vy: 2), count: 8)
for i in 0..<8 { ps[i] = P(x: Int64(i) * 100, y: Int64(i) * 37, vx: 1, vy: 2) }
for _ in 0..<2000000 {
    for i in 0..<8 {
        for j in 0..<8 where i != j { ps[i] = interact(ps[i], ps[j]) }
    }
    for i in 0..<8 { ps[i].x &+= ps[i].vx; ps[i].y &+= ps[i].vy }
}
var h: Int64 = 0
for p in ps { h &+= p.x &+ p.y &+ p.vx &+ p.vy }
print(h)
