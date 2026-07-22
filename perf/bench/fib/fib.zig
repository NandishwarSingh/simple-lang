const std = @import("std");
fn fib(n: i64) i64 {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
pub fn main() !void {
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{fib(42)});
    _ = try std.posix.write(1, _ps);
}
