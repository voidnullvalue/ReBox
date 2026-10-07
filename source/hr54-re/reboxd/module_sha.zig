// Streaming SHA-256 adapter using Zig's maintained standard crypto implementation.
// The receiver's minimal busybox has no sha256sum applet. No shell is involved.
const std = @import("std");
pub fn main() !void {
    const args = try std.process.argsAlloc(std.heap.page_allocator);
    defer std.process.argsFree(std.heap.page_allocator, args);
    if (args.len != 2 or args[1].len > 1023) return error.InvalidArguments;
    const input = try std.fs.cwd().openFile(args[1], .{});
    defer input.close();
    // statx is unavailable on the receiver's Linux 3.3 kernel.
    const stat = try std.posix.fstat(input.handle);
    if (!std.posix.S.ISREG(stat.mode) or stat.size < 0 or stat.size > 64 * 1024 * 1024) return error.InvalidInput;
    var hash = std.crypto.hash.sha2.Sha256.init(.{});
    var buffer: [32768]u8 = undefined;
    var total: usize = 0;
    while (true) {
        const count = try input.read(&buffer);
        if (count == 0) break;
        total += count;
        if (total > 64 * 1024 * 1024) return error.InputTooLarge;
        hash.update(buffer[0..count]);
    }
    var digest: [32]u8 = undefined;
    hash.final(&digest);
    const hex = std.fmt.bytesToHex(digest, .lower);
    var reply: [1090]u8 = undefined;
    const line = try std.fmt.bufPrint(&reply, "{s}  {s}\n", .{ hex, args[1] });
    try (std.fs.File{ .handle = std.posix.STDOUT_FILENO }).writeAll(line);
}
