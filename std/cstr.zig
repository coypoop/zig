const std = @import("index.zig");
const builtin = @import("builtin");
const debug = std.debug;
const mem = std.mem;
const testing = std.testing;

pub const line_sep = switch (builtin.os) {
    builtin.Os.windows => "\r\n",
    else => "\n",
};

/// Deprecated, use mem.len
pub fn len(ptr: [*]const u8) usize {
    return mem.len(u8, ptr);
}

pub fn cmp(a: [*]const u8, b: [*]const u8) i8 {
    var index: usize = 0;
    while (a[index] == b[index] and a[index] != 0) : (index += 1) {}
    if (a[index] > b[index]) {
        return 1;
    } else if (a[index] < b[index]) {
        return -1;
    } else {
        return 0;
    }
}

/// Deprecated, use mem.toSliceConst
pub fn toSliceConst(str: [*]const u8) []const u8 {
    return mem.toSliceConst(u8, str);
}

/// Deprecated, use mem.toSlice
pub fn toSlice(str: [*]u8) []u8 {
    return mem.toSlice(u8, str);
}

test "cstr fns" {
    comptime testCStrFnsImpl();
    testCStrFnsImpl();
}

fn testCStrFnsImpl() void {
    testing.expect(cmp(c"aoeu", c"aoez") == -1);
    testing.expect(len(c"123456789") == 9);
}

/// Returns a mutable slice with 1 more byte of length which is a null byte.
/// Caller owns the returned memory.
pub fn addNullByte(allocator: *mem.Allocator, slice: []const u8) ![]u8 {
    const result = try allocator.alloc(u8, slice.len + 1);
    mem.copy(u8, result, slice);
    result[slice.len] = 0;
    return result;
}

pub const NullTerminated2DArray = struct {
    allocator: *mem.Allocator,
    byte_count: usize,
    ptr: ?[*]?[*]u8,

    /// Takes N lists of strings, concatenates the lists together, and adds a null terminator
    /// Caller must deinit result
    pub fn fromSlices(allocator: *mem.Allocator, slices: []const []const []const u8) !NullTerminated2DArray {
        var new_len: usize = 1; // 1 for the list null
        var byte_count: usize = 0;
        for (slices) |slice| {
            new_len += slice.len;
            for (slice) |inner| {
                byte_count += inner.len;
            }
            byte_count += slice.len; // for the null terminators of inner
        }

        const index_size = @sizeOf(usize) * new_len; // size of the ptrs
        byte_count += index_size;

        const buf = try allocator.alignedAlloc(u8, @alignOf(?*u8), byte_count);
        errdefer allocator.free(buf);

        var write_index = index_size;
        const index_buf = @bytesToSlice(?[*]u8, buf);

        var i: usize = 0;
        for (slices) |slice| {
            for (slice) |inner| {
                index_buf[i] = buf.ptr + write_index;
                i += 1;
                mem.copy(u8, buf[write_index..], inner);
                write_index += inner.len;
                buf[write_index] = 0;
                write_index += 1;
            }
        }
        index_buf[i] = null;

        return NullTerminated2DArray{
            .allocator = allocator,
            .byte_count = byte_count,
            .ptr = @ptrCast(?[*]?[*]u8, buf.ptr),
        };
    }

    pub fn deinit(self: *NullTerminated2DArray) void {
        const buf = @ptrCast([*]u8, self.ptr);
        self.allocator.free(buf[0..self.byte_count]);
    }
};
