#pragma once
// Parse support-library atomics before redefining only generated call sites.
#include "ppc_recomp_shared.h"
#include "runtime_guest_atomic_write.h"

#define __sync_bool_compare_and_swap(destination, expected, desired) \
    RuntimeGeneratedCompareExchange(base, (destination), (expected), (desired), __FILE__, __LINE__)
