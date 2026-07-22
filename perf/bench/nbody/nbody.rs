// IDIOMATIC-CLASS: natural Rust. The borrow checker forbids two mutable
// references into the array at once, so Rust programmers write this with
// Copy values — the same no-aliasing win Simple gets from value semantics.
#[derive(Clone, Copy)]
struct P {
    x: i64,
    y: i64,
    vx: i64,
    vy: i64,
}

fn interact(a: P, b: P) -> P {
    let dx = b.x - a.x;
    let dy = b.y - a.y;
    P { x: a.x, y: a.y, vx: a.vx + dx / 16, vy: a.vy + dy / 16 }
}

fn main() {
    let mut ps = [P { x: 0, y: 0, vx: 1, vy: 2 }; 8];
    for i in 0..8usize {
        ps[i] = P { x: i as i64 * 100, y: i as i64 * 37, vx: 1, vy: 2 };
    }
    for _ in 0..2000000 {
        for i in 0..8usize {
            for j in 0..8usize {
                if i != j {
                    ps[i] = interact(ps[i], ps[j]);
                }
            }
        }
        for i in 0..8usize {
            ps[i].x += ps[i].vx;
            ps[i].y += ps[i].vy;
        }
    }
    let mut h: i64 = 0;
    for i in 0..8usize {
        h += ps[i].x + ps[i].y + ps[i].vx + ps[i].vy;
    }
    println!("{}", h);
}
