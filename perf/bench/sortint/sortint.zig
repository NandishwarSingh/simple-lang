const std = @import("std");
pub fn main() !void {
    var a: [800]i64 = undefined;
    var check: i64 = 0;
    var seed: u64 = 987654321;
    for (0..600) |_| {
        for (0..800) |i| {
            seed = seed *% 6364136223846793005 +% 1442695040888963407;
            a[i] = @intCast(seed >> 40);
        }
        var i: usize = 1;
        while (i < 800) : (i += 1) {
            const v = a[i];
            var j: i64 = @as(i64, @intCast(i)) - 1;
            while (j >= 0 and a[@intCast(j)] > v) : (j -= 1) {
                a[@intCast(j + 1)] = a[@intCast(j)];
            }
            a[@intCast(j + 1)] = v;
        }
        check += a[0] + a[799];
    }
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{check});
    _ = try std.posix.write(1, _ps);
}
