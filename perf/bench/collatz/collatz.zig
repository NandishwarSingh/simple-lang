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
pub fn main() !void {
    var total: i64 = 0;
    var i: i64 = 1;
    while (i < 5000000) : (i += 1) total += steps(i);
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{total});
    _ = try std.posix.write(1, _ps);
}
