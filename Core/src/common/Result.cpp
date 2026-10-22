// Result<T> is header-only (pure inline). This translation unit is kept so the
// build always has a stable .cpp anchor for the common module; future error
// string tables can live here.

#include "common/Result.h"

namespace pnx {
namespace {
// Anchor: ensures the module compiles even if no other TU uses Result.
[[maybe_unused]] constexpr ResultCode kAnchor = ResultOk;
} // namespace
} // namespace pnx
