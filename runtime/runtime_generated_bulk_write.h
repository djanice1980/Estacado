#pragma once

// Forced only into generated translation units containing memory clears. Load
// all generated dependencies before the local macro, so CRT/SIMDe declarations
// and inline implementation bodies retain their normal memset semantics.
#include "ppc_recomp_shared.h"
#include "runtime_guest_bulk_write.h"

// Existing generated dcbz/dcbzl calls become tracked writes without modifying
// generated files or regenerating the title. Handwritten runtime code never
// includes this header. Arguments are evaluated once by the function call.
#define memset(destination, value, bytes) \
    RuntimeGeneratedMemset(base, (destination), (value), (bytes), __FILE__, __LINE__)
