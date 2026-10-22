#include "common/Result.h"

// Result is header-only; this translation unit keeps it visible to the
// build and is a placeholder for future error-string tables.
namespace pnx {
namespace Result {
    // Intentionally empty anchor symbol.
    inline ResultCode Anchor() { return Unimplemented; }
} // namespace Result
} // namespace pnx
