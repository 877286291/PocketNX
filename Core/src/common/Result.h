#pragma once

#include <cstdint>
#include <utility>

namespace pnx {

// Switch-style result code. 0 == success. Mirrors the convention used across
// the NX OS service layer so unimplemented calls can surface as clean results.
using ResultCode = uint32_t;
constexpr ResultCode ResultOk = 0;

// Module / error codes. Kept minimal for M0; expand as services are added.
namespace Result {
    constexpr ResultCode ModuleCPU  = 0x0100;
    constexpr ResultCode ModuleGPU  = 0x0200;
    constexpr ResultCode ModuleOS   = 0x0300;
    constexpr ResultCode ModuleFS   = 0x0500;

    constexpr ResultCode Unimplemented = 0xFFFF;
    constexpr ResultCode NotFound      = 0xFFF1;
    constexpr ResultCode InvalidArg    = 0xFFF2;
    constexpr ResultCode AccessDenied  = 0xFFF3;
    constexpr ResultCode Unsupported   = 0xFFF4;
}

// Lightweight tagged result. Use for fallible kernel operations.
template <typename T>
struct Result {
    T value{};
    ResultCode code = ResultOk;

    static Result<T> Ok(T v) { return Result<T>{std::move(v), ResultOk}; }
    static Result<T> Err(ResultCode c) { return Result<T>{T{}, c}; }

    bool IsOk() const { return code == ResultOk; }
    explicit operator bool() const { return IsOk(); }
    T& Unwrap() { return value; }
};

// Specialization for operations that only succeed or fail.
template <>
struct Result<void> {
    ResultCode code = ResultOk;

    static Result<void> Ok() { return Result<void>{ResultOk}; }
    static Result<void> Err(ResultCode c) { return Result<void>{c}; }

    bool IsOk() const { return code == ResultOk; }
    explicit operator bool() const { return IsOk(); }
};

using VoidResult = Result<void>;

} // namespace pnx
