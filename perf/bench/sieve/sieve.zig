const std = @import("std");
const N: usize = 2000000;
var flags: [N]bool = undefined;
pub fn main() !void {
    var count: i64 = 0;
    for (0..10) |_| {
        count = 0;
        @memset(&flags, true);
        var i: usize = 2;
        while (i < N) : (i += 1) {
            if (flags[i]) {
                count += 1;
                var j = i + i;
                while (j < N) : (j += i) flags[j] = false;
            }
        }
    }
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{count});
    _ = try std.posix.write(1, _ps);
}
