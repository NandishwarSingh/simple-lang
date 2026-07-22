const std = @import("std");
var a: [200][200]i64 = undefined;
var b: [200][200]i64 = undefined;
var c: [200][200]i64 = undefined;
pub fn main() !void {
    for (0..200) |i| for (0..200) |j| {
        a[i][j] = @intCast((i + j) % 7);
        b[i][j] = @intCast((i * j) % 5);
        c[i][j] = 0;
    };
    for (0..20) |_| for (0..200) |i| for (0..200) |k| {
        const aik = a[i][k];
        for (0..200) |j| c[i][j] += aik * b[k][j];
    };
    var sum: i64 = 0;
    for (0..200) |i| sum += c[i][i];
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{sum});
    _ = try std.posix.write(1, _ps);
}
