const std = @import("../../index.zig");

export var _tls_index: u32 = std.os.windows.TLS_OUT_OF_INDEXES;
export var _tls_start: u8 linksection(".tls") = 0;
export var _tls_end: u8 linksection(".tls$ZZZ") = 0;
export var __xl_a: std.os.windows.PIMAGE_TLS_CALLBACK linksection(".CRT$XLA") = null;
export var __xl_z: std.os.windows.PIMAGE_TLS_CALLBACK linksection(".CRT$XLZ") = null;

// TODO this is how I would like it to be expressed
// TODO also note, ReactOS has a +1 on StartAddressOfRawData and AddressOfCallBacks. Investigate
// why they do that.
//export const _tls_used linksection(".rdata$T") = std.os.windows.IMAGE_TLS_DIRECTORY {
//    .StartAddressOfRawData = @ptrToInt(&_tls_start),
//    .EndAddressOfRawData = @ptrToInt(&_tls_end),
//    .AddressOfIndex = @ptrToInt(&_tls_index),
//    .AddressOfCallBacks = @ptrToInt(__xl_a),
//    .SizeOfZeroFill = 0,
//    .Characteristics = 0,
//};
// This is the workaround because we can't do @ptrToInt at comptime like that.
pub const IMAGE_TLS_DIRECTORY = extern struct {
    StartAddressOfRawData: *c_void,
    EndAddressOfRawData: *c_void,
    AddressOfIndex: *c_void,
    AddressOfCallBacks: *c_void,
    SizeOfZeroFill: u32,
    Characteristics: u32,
};
export const _tls_used linksection(".rdata$T") = IMAGE_TLS_DIRECTORY {
    .StartAddressOfRawData = &_tls_start,
    .EndAddressOfRawData = &_tls_end,
    .AddressOfIndex = &_tls_index,
    .AddressOfCallBacks = &__xl_a,
    .SizeOfZeroFill = 0,
    .Characteristics = 0,
};
