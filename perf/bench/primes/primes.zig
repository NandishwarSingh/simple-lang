const std = @import("std");
fn isPrime(n: i64) bool {
    if (n < 2) return false;
    var d: i64 = 2;
    while (d * d <= n) : (d += 1) {
        if (@rem(n, d) == 0) return false;
    }
    return true;
}
pub fn main() !void {
    var count: i64 = 0;
    var n: i64 = 2;
    while (n < 3000000) : (n += 1) {
        if (isPrime(n)) count += 1;
    }
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{count});
    _ = try std.posix.write(1, _ps);
}
