const std = @import("std");
const P = struct { x: i64, y: i64, vx: i64, vy: i64 };
fn interact(a: P, b: P) P {
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    return P{ .x = a.x, .y = a.y, .vx = a.vx + @divTrunc(dx, 16), .vy = a.vy + @divTrunc(dy, 16) };
}
pub fn main() !void {
    var ps: [8]P = undefined;
    for (&ps, 0..) |*p, i| {
        p.* = P{ .x = @as(i64, @intCast(i)) * 100, .y = @as(i64, @intCast(i)) * 37, .vx = 1, .vy = 2 };
    }
    var s: usize = 0;
    while (s < 2000000) : (s += 1) {
        var i: usize = 0;
        while (i < 8) : (i += 1) {
            var j: usize = 0;
            while (j < 8) : (j += 1) {
                if (i != j) ps[i] = interact(ps[i], ps[j]);
            }
        }
        for (&ps) |*p| { p.x += p.vx; p.y += p.vy; }
    }
    var h: i64 = 0;
    for (ps) |p| h += p.x + p.y + p.vx + p.vy;
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{h});
    _ = try std.posix.write(1, _ps);
}
