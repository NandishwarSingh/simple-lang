const std = @import("std");
fn steps(n0: i64) i64 {
    var n = n0;
    var s: i64 = 0;
    while (n != 1) {
        if (@rem(n, 2) == 0) { n = @divTrunc(n, 2); } else { n = 3 * n + 1; }
        s += 1;
    }
    return s;
}
fn worker(lo: i64, hi: i64, out: *i64) void {
    var t: i64 = 0;
    var i = lo;
    while (i < hi) : (i += 1) t += steps(i);
    out.* = t;
}
pub fn main() !void {
    var totals: [8]i64 = .{0} ** 8;
    var threads: [8]std.Thread = undefined;
    const chunk: i64 = 625000;
    for (0..8) |w| {
        const lo: i64 = if (w == 0) 1 else @as(i64, @intCast(w)) * chunk;
        const hi: i64 = (@as(i64, @intCast(w)) + 1) * chunk;
        threads[w] = try std.Thread.spawn(.{}, worker, .{ lo, hi, &totals[w] });
    }
    var total: i64 = 0;
    for (0..8) |w| { threads[w].join(); total += totals[w]; }
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{total});
    _ = try std.posix.write(1, _ps);
}
