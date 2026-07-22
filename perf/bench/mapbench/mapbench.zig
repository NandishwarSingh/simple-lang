const std = @import("std");

pub fn main() !void {
    const alloc = std.heap.c_allocator;

    var a = std.AutoHashMap(i64, i64).init(alloc);
    defer a.deinit();
    var i: i64 = 0;
    while (i < 10000000) : (i += 1) {
        try a.put(@mod(i * 2654435761, 4000037), i);
    }
    var suma: i64 = 0;
    i = 0;
    while (i < 10000000) : (i += 1) {
        const k = @mod(i * 7919, 4000037);
        if (a.get(k)) |v| suma += v;
    }
    i = 0;
    while (i < 1000000) : (i += 1) {
        _ = a.remove(@mod(i * 31, 4000037));
    }
    suma += @as(i64, @intCast(a.count())) * 17;

    var wc = std.StringHashMap(i64).init(alloc);
    defer wc.deinit();
    var buf: [32]u8 = undefined;
    i = 0;
    while (i < 1500000) : (i += 1) {
        const w = try std.fmt.bufPrint(&buf, "w{d}", .{@mod(i * 131, 9973)});
        if (wc.getPtr(w)) |p| {
            p.* += 1;
        } else {
            const owned = try alloc.dupe(u8, w);
            try wc.put(owned, 1);
        }
    }
    var sumb: i64 = 0;
    i = 0;
    while (i < 9973) : (i += 1) {
        const w = try std.fmt.bufPrint(&buf, "w{d}", .{i});
        if (wc.get(w)) |v| sumb += v * (i + 1);
    }
    var sumc: i64 = 0;
    var it = wc.valueIterator();
    while (it.next()) |v| sumc += v.*;

    var obuf: [32]u8 = undefined;
    const out = try std.fmt.bufPrint(&obuf, "{d}\n", .{suma + sumb * 3 + sumc});
    _ = try std.posix.write(1, out);
}
