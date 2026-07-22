const std = @import("std");

const N: usize = 8192;

var x: [N]f64 = undefined;
var y: [N]f64 = undefined;
var a: [128][128]f64 = undefined;
var b: [128][128]f64 = undefined;
var c: [128][128]f64 = undefined;

pub fn main() !void {
    for (0..N) |i| {
        x[i] = @as(f64, @floatFromInt((i * 7) % 1000)) / 1000.0;
        y[i] = @as(f64, @floatFromInt((i * 13) % 1000)) / 1000.0;
    }

    var step: usize = 0;
    while (step < 120000) : (step += 1) {
        for (0..N) |i| {
            x[i] = x[i] * 0.99 + y[i] * 0.01;
            y[i] = y[i] * 0.99 + x[i] * 0.01;
        }
    }

    step = 0;
    while (step < 70000) : (step += 1) {
        for (0..N) |i| {
            const v = x[i];
            x[i] = ((v * 0.5 + 0.25) * v + 0.1) * 0.5 + y[i] * 0.5;
        }
    }

    for (0..128) |i| {
        for (0..128) |j| {
            a[i][j] = @as(f64, @floatFromInt((i * 7 + j) % 100)) / 100.0;
            b[i][j] = @as(f64, @floatFromInt((i * 5 + j * 3) % 100)) / 100.0;
        }
    }
    var trace: f64 = 0.0;
    var rep: usize = 0;
    while (rep < 200) : (rep += 1) {
        for (0..128) |i| {
            for (0..128) |j| c[i][j] = 0.0;
        }
        for (0..128) |i| {
            for (0..128) |k| {
                const aik = a[i][k];
                for (0..128) |j| {
                    c[i][j] = c[i][j] + aik * b[k][j];
                }
            }
        }
        for (0..128) |i| {
            trace = trace + c[i][i];
        }
    }

    var s: f64 = 0.0;
    for (0..N) |i| {
        s = s + x[i] + y[i];
    }
    s = s + trace;
    const q: i64 = @intFromFloat(s * 1000000.0);
    var buf: [32]u8 = undefined;
    const out = try std.fmt.bufPrint(&buf, "{d}\n", .{@mod(q, 1000000000000)});
    _ = try std.posix.write(1, out);
}
