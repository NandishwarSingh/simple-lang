const std = @import("std");
const Slot = struct {
    m: std.Thread.Mutex = .{},
    c: std.Thread.Condition = .{},
    v: i64 = 0,
    full: bool = false,
    fn send(s: *Slot, val: i64) void {
        s.m.lock(); defer s.m.unlock();
        while (s.full) s.c.wait(&s.m);
        s.v = val; s.full = true; s.c.signal();
    }
    fn recv(s: *Slot) i64 {
        s.m.lock(); defer s.m.unlock();
        while (!s.full) s.c.wait(&s.m);
        const r = s.v; s.full = false; s.c.signal();
        return r;
    }
};
var ping = Slot{};
var pong = Slot{};
fn ponger() void {
    while (true) {
        const v = ping.recv();
        if (v == -1) return;
        pong.send(v + 1);
    }
}
pub fn main() !void {
    const t = try std.Thread.spawn(.{}, ponger, .{});
    var total: i64 = 0;
    var i: i64 = 0;
    while (i < 100000) : (i += 1) { ping.send(i); total += pong.recv(); }
    ping.send(-1);
    t.join();
    var _pbuf: [64]u8 = undefined;
    const _ps = try std.fmt.bufPrint(&_pbuf, "{d}\n", .{total});
    _ = try std.posix.write(1, _ps);
}
