const std = @import("std");

fn rngStep(s: u64) u64 {
    return s *% 6364136223846793005 +% 1442695040888963407;
}
fn rngVal(s: u64) i64 {
    return @bitCast(s >> 33);
}

const Body = struct { x: i64, y: i64, z: i64, vx: i64, vy: i64, vz: i64 };

fn interact(a: Body, b: Body) Body {
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const dz = b.z - a.z;
    const d2 = dx * dx + dy * dy + dz * dz + 1;
    const inv = @divTrunc(@as(i64, 1000000), (@divTrunc(d2, 1000) + 1));
    return Body{
        .x = a.x, .y = a.y, .z = a.z,
        .vx = a.vx + @divTrunc(dx * inv, 100000),
        .vy = a.vy + @divTrunc(dy * inv, 100000),
        .vz = a.vz + @divTrunc(dz * inv, 100000),
    };
}

fn phaseA() i64 {
    var bs: [32]Body = undefined;
    var r: u64 = 88172645463325252;
    for (0..32) |i| {
        r = rngStep(r);
        const x = @mod(rngVal(r), 1000);
        r = rngStep(r);
        const y = @mod(rngVal(r), 1000);
        r = rngStep(r);
        const z = @mod(rngVal(r), 1000);
        bs[i] = Body{ .x = x, .y = y, .z = z, .vx = 0, .vy = 0, .vz = 0 };
    }
    var step: usize = 0;
    while (step < 30000) : (step += 1) {
        for (0..32) |i| {
            for (0..32) |j| {
                if (i != j) bs[i] = interact(bs[i], bs[j]);
            }
        }
    }
    var sum: i64 = 0;
    for (0..32) |i| sum += bs[i].vx + bs[i].vy + bs[i].vz;
    return sum;
}

fn phaseB(alloc: std.mem.Allocator) !u64 {
    const N: usize = 500000;
    const items = try alloc.alloc([]u8, N);
    defer {
        for (items) |s| alloc.free(s);
        alloc.free(items);
    }
    var k: i64 = 0;
    while (k < 500000) : (k += 1) {
        items[@intCast(k)] = try std.fmt.allocPrint(alloc, "k{d}_{d}", .{ k, @mod(k * 13, 97) });
    }
    var acc: u64 = 0;
    for (items) |s| {
        var h: u64 = 14695981039346656037;
        for (s) |c| h = (h ^ @as(u64, c)) *% 1099511628211;
        acc ^= h;
    }
    return acc + @as(u64, N);
}

fn phaseC() i64 {
    var a: [25000]i64 = undefined;
    var r: u64 = 99887766554433;
    for (0..25000) |i| {
        r = rngStep(r);
        a[i] = @mod(rngVal(r), 1000000);
    }
    var i: usize = 1;
    while (i < 25000) : (i += 1) {
        const v = a[i];
        var j: i64 = @as(i64, @intCast(i)) - 1;
        while (j >= 0 and a[@intCast(j)] > v) : (j -= 1) {
            a[@intCast(j + 1)] = a[@intCast(j)];
        }
        a[@intCast(j + 1)] = v;
    }
    var s: i64 = 0;
    for (0..25000) |ii| s += a[ii] * @as(i64, @intCast(ii));
    return s;
}

fn fib(n: i64) i64 {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
fn phaseD() i64 {
    return fib(40);
}

fn phaseE() i64 {
    var a: [100][100]i64 = undefined;
    var b: [100][100]i64 = undefined;
    for (0..100) |i| {
        for (0..100) |j| {
            a[i][j] = @intCast(@mod(i * 7 + j, 13));
            b[i][j] = @intCast(@mod(i * 5 + j * 3, 11));
        }
    }
    var acc: i64 = 0;
    var rep: usize = 0;
    while (rep < 150) : (rep += 1) {
        var c = std.mem.zeroes([100][100]i64);
        for (0..100) |i| {
            for (0..100) |k| {
                const aik = a[i][k];
                for (0..100) |j| c[i][j] += aik * b[k][j];
            }
        }
        for (0..100) |i| acc += c[i][i];
    }
    return acc;
}

pub fn main() !void {
    const alloc = std.heap.c_allocator;
    const a = phaseA();
    const b = try phaseB(alloc);
    const c = phaseC();
    const d = phaseD();
    const e = phaseE();
    var sum: u64 = 0;
    sum +%= @bitCast(a);
    sum ^= b;
    sum +%= @bitCast(c);
    sum ^= @as(u64, @bitCast(d));
    sum +%= @bitCast(e);
    var buf: [32]u8 = undefined;
    const out = try std.fmt.bufPrint(&buf, "{d}\n", .{sum % 1000000000000});
    _ = try std.posix.write(1, out);
}
