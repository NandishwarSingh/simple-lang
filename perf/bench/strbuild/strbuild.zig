const std = @import("std");
pub fn main() !void {
    const alloc = std.heap.c_allocator;
    var total: i64 = 0;
    var i: usize = 0;
    while (i < 1000000) : (i += 1) {
        var s = try alloc.alloc(u8, 0);
        var j: usize = 0;
        while (j < 8) : (j += 1) {
            const t = try alloc.alloc(u8, s.len + 7);
            @memcpy(t[0..s.len], s);
            @memcpy(t[s.len..], "abcdefg");
            alloc.free(s);
            s = t;
        }
        total += @intCast(s.len);
        alloc.free(s);
    }
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{total});
    _ = try std.posix.write(1, _ps);
}
