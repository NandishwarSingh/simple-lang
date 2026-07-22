const std = @import("std");
fn popcount(x0: u64) i64 {
    var x = x0;
    var n: i64 = 0;
    while (x != 0) { x = x & (x - 1); n += 1; }
    return n;
}
fn mix(h0: u64) u64 {
    var h = h0;
    h = h ^ (h >> 33);
    h = h *% 18397679294719823053;
    h = h ^ (h >> 29);
    return h;
}
pub fn main() !void {
    var total: i64 = 0;
    var h: u64 = 12345;
    for (0..20000000) |_| { h = mix(h); total += popcount(h); }
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{total});
    _ = try std.posix.write(1, _ps);
}
