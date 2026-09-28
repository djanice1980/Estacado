#pragma once

#include "guest_physical_write_tracker.h"
#include "runtime_guest_physical_range.h"
#include "runtime_mmio.h"
#include "runtime_source_memory_coordinator.h"

#include <cstdint>
#include <intrin.h>

void RuntimeNotifyGuestPhysicalWrite(uint32_t guest_address, uint32_t length,
                                     const char* source_file,
                                     uint32_t source_line) noexcept;
// Page-dirty state shared with the GPU submission boundary (runtime_graphics.cpp).
extern GuestPhysicalWriteTracker g_guest_physical_writes;

// V285 store hot path: a store to a physical page that is already pending
// (written since the last submission drain) needs no further publication; see
// GuestPhysicalWriteTracker for why skipping the locked RMW is safe. Any other
// case, including page-crossing writes and inactive graphics, takes the
// original notification unchanged.
__forceinline bool RuntimeGuestPhysicalWriteAlreadyPending(uint32_t address,
                                                           uint32_t length) noexcept {
    uint32_t physical = 0;
    return RuntimeCanonicalGuestPhysicalRange(address, length, physical) &&
           ((physical ^ (physical + length - 1u)) >> GuestPhysicalWriteTracker::kPageShift) == 0 &&
           g_guest_physical_writes.PagePending(physical);
}
// Guest thread stacks (runtime_threads.cpp: allocated downward from
// 0x7EF00000) and the rest of 0x70000000-0x7EFFFFFF are ordinary RAM: never
// MMIO, never a GPU-visible physical alias, never source-tracked. Stores there
// take the plain byte-swapped store inline, exactly what the helpers would do.
constexpr bool RuntimeGuestPlainStackAddress(uint32_t address, uint32_t length) noexcept {
    return address - 0x70000000u < 0x0F000000u - length;
}
static_assert(RuntimeGuestPlainStackAddress(0x7EEFFFF0u, 4u));
static_assert(!RuntimeGuestPlainStackAddress(0x7EFFFFFEu, 4u));
static_assert(!RuntimeGuestPlainStackAddress(0x7F000000u, 1u));
static_assert(!RuntimeGuestPlainStackAddress(0x6FFFFFFCu, 4u));

// Out-of-line variant for the inlined (non-U32) store sites, keeping their
// code size unchanged.
__declspec(noinline) inline void RuntimeNotifyGuestPhysicalWriteChecked(
        uint32_t address, uint32_t length) noexcept {
    if (!RuntimeGuestPhysicalWriteAlreadyPending(address, length))
        RuntimeNotifyGuestPhysicalWrite(address, length, nullptr, 0);
}
void RuntimeObserveAllocatorHeaderStore(uint8_t* base, uint32_t address, uint32_t width,
                                        uint64_t value, void* caller);
void RuntimeObserveItemSmartPointerStore(uint8_t* base, uint32_t address,
                                         uint32_t value, void* caller);

// The M5 allocator/item-pointer investigation proved that the apparent host
// access violation was an unowned inline switch target, not a guest-store
// corruption. Keep the observers available for a future targeted build, but
// compile their predicates out of every ordinary generated store now that the
// investigation is complete.
constexpr bool kCompletedAllocatorStoreTraceEnabled = false;

// XenonRecomp currently identifies MMIO stores only when the following
// instruction is eieio, and doesn't tag MMIO loads. The title reads Xenos
// display/interrupt registers without that sequence (for example,
// D1MODE_VBLANK_VLINE_STATUS at 0x7FC86544), so every 32-bit access to the
// architected Xenos register aperture must cross the runtime MMIO boundary
// even when the generated operation is PPC_LOAD/STORE rather than PPC_MM_*.
inline bool RuntimeIsArchitectedMmioAddress(uint32_t address) noexcept {
    return address - 0x7FC80000u <= 0x0007FFFFu ||
           address - 0x7FEA0000u <= 0x0000FFFFu;
}

// This is a conservative, allocation-free prefilter for the exact alias
// ranges accepted by runtime_graphics.cpp's GuestAliasToPhysical. It exists
// only to avoid calling the graphics invalidation boundary for title image and
// other virtual addresses that cannot alias GPU-visible physical RAM. The
// callee remains authoritative and validates the translated span again.
constexpr bool RuntimeGuestStoreMayReachPhysicalMemory(uint32_t address,
                                                       uint32_t length) noexcept {
    if (!length) return false;
    if (address >= 0x7F000000u && address < 0x80000000u) return true;
    if (address >= 0xA0000000u && address < 0xE0000000u) {
        return uint64_t(address & 0x1FFFFFFFu) + length <= 0x20000000ull;
    }
    return address >= 0xE0000000u && address < 0xFFD00000u;
}

static_assert(RuntimeGuestStoreMayReachPhysicalMemory(0x7F000000u, 1u));
static_assert(RuntimeGuestStoreMayReachPhysicalMemory(0x7FFFFFFFu, 16u));
static_assert(!RuntimeGuestStoreMayReachPhysicalMemory(0x80000000u, 4u));
static_assert(!RuntimeGuestStoreMayReachPhysicalMemory(0x9FFFFFFFu, 4u));
static_assert(RuntimeGuestStoreMayReachPhysicalMemory(0xA0000000u, 16u));
static_assert(RuntimeGuestStoreMayReachPhysicalMemory(0xDFFFFFFFu, 1u));
static_assert(!RuntimeGuestStoreMayReachPhysicalMemory(0xDFFFFFFFu, 2u));
static_assert(RuntimeGuestStoreMayReachPhysicalMemory(0xE0000000u, 16u));
static_assert(!RuntimeGuestStoreMayReachPhysicalMemory(0xFFD00000u, 1u));

inline bool RuntimeAllocatorHeaderStoreOverlaps(uint32_t address, uint32_t width) noexcept {
    constexpr uint64_t kAllocatorHeaderFirst = 0xBF000080u;
    constexpr uint64_t kAllocatorHeaderEnd = 0xBF000098u;
    constexpr uint64_t kAllocatorFirstPoolFreeHead = 0xBF000588u;
    return (uint64_t(address) < kAllocatorHeaderEnd &&
            uint64_t(address) + width > kAllocatorHeaderFirst) ||
           (uint64_t(address) <= kAllocatorFirstPoolFreeHead &&
            uint64_t(address) + width > kAllocatorFirstPoolFreeHead);
}

inline bool RuntimeAllocatorHeaderStoreIsSuspicious(uint32_t address, uint32_t width,
                                                    uint64_t value) noexcept {
    // Capture the provenance of any link that inserts the live allocator
    // object into its first fixed-node pool page, not merely the later head
    // update that exposes it for allocation.
    if (width == 4u && (value == 0xBF000080u || value == 0xBF000580u) &&
        address >= 0xBF000590u && address < 0xBF00E000u &&
        ((address - 0xBF000590u) % 0x24u) == 0u) {
        return true;
    }
    if (!RuntimeAllocatorHeaderStoreOverlaps(address, width)) return false;
    if (width != 4u || (address & 3u) != 0) return true;
    switch (address) {
    case 0xBF000080u:
        // Base and derived allocator constructors install these two vtables.
        return value != 0u && value != 0x82065B24u && value != 0x8206573Cu;
    case 0xBF000084u:
        // Ordinary code only performs the constructor's zero initialization;
        // all live lock-count transitions use lwarx/stwcx.
        return value != 0u;
    case 0xBF000088u: return value > 2u;
    case 0xBF00008Cu: return value > 64u;
    case 0xBF000090u: return value > 64u;
    case 0xBF000094u: return value > 0x10000u;
    // The first 36-byte management-node pool is rooted here. Linking the live
    // allocator object into this free list is the earliest known corruption.
    case 0xBF000588u:
        return value != 0u &&
               !(value >= 0xBF000590u && value < 0xBF00E000u &&
                 ((value - 0xBF000590u) % 0x24u) == 0u);
    default: return true;
    }
}

inline bool RuntimeItemSmartPointerStoreIsRelevant(uint8_t* base,
                                                   uint32_t address,
                                                   uint32_t value) noexcept {
    constexpr uint32_t kItemVtable = 0x82095798u;
    constexpr uint32_t kTargetContainer = 0xAE1C49F0u;
    // The dynamically implicated objects live in the title heap. Restricting
    // this diagnostic to that aperture avoids an extra guest load on ordinary
    // stack, static-data, physical-alias and MMIO stores.
    if (address < 0xA0000008u || address >= 0xC0000000u ||
        (address & 3u) != 0u) {
        return false;
    }
    // Observe either an ownership-bearing item+0x0C write, or the later
    // item+0x08 link that attaches an already-populated item to the implicated
    // long-lived container. The latter identifies the exact clone/insert path
    // without changing title state.
    if (address >= 0xA000000Cu) {
        const uint32_t raw = *reinterpret_cast<volatile uint32_t*>(
            base + address - 12u);
        if (__builtin_bswap32(raw) == kItemVtable) return true;
    }
    if (value == kTargetContainer) {
        const uint32_t raw = *reinterpret_cast<volatile uint32_t*>(
            base + address - 8u);
        return __builtin_bswap32(raw) == kItemVtable;
    }
    return false;
}

inline uint32_t RuntimePpcLoadU32(uint8_t* base, uint32_t address) {
    if (RuntimeIsArchitectedMmioAddress(address)) {
        return RuntimeMmioLoadU32(base, address);
    }
    return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + address));
}

namespace runtime_memory_access_detail {
template <typename T>
inline void StoreScalarPayload(uint8_t* base, uint32_t address, T value) {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
    if constexpr (sizeof(T) == 2) value = __builtin_bswap16(value);
    if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
    if constexpr (sizeof(T) == 8) value = __builtin_bswap64(value);
    *reinterpret_cast<volatile T*>(base + address) = value;
    if (RuntimeGuestStoreMayReachPhysicalMemory(address, sizeof(T))) {
        if constexpr (sizeof(T) == 4) {
            // Only reached through the shared RuntimePpcStoreU32 boundary:
            // the pending check is inlined there once, not at every site.
            if (!RuntimeGuestPhysicalWriteAlreadyPending(address, sizeof(T)))
                RuntimeNotifyGuestPhysicalWrite(address, sizeof(T), nullptr, 0);
        } else {
            RuntimeNotifyGuestPhysicalWriteChecked(address, sizeof(T));
        }
    }
}

template <typename T>
__declspec(noinline) inline void StoreTrackedScalar(uint8_t* base, uint32_t address, T value) {
    std::optional<uint32_t> stored_u32;
    if constexpr (sizeof(T) == 4) stored_u32 = value;
    const RuntimeGuestSourceWriteScope source_write(address, sizeof(T), stored_u32);
    StoreScalarPayload(base, address, value);
}
}  // namespace runtime_memory_access_detail

template <typename T>
__forceinline void RuntimePpcStoreScalar(uint8_t* base, uint32_t address, T value) {
    if (RuntimeGuestPlainStackAddress(address, sizeof(T))) {
        if constexpr (sizeof(T) == 2) value = __builtin_bswap16(value);
        if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
        if constexpr (sizeof(T) == 8) value = __builtin_bswap64(value);
        *reinterpret_cast<volatile T*>(base + address) = value;
        return;
    }
    // Freeze mode before the payload exactly as the original scope did. Once
    // disabled, no live reconfiguration can expose this store to snapshots.
    // Keep tracking's RAII storage and cleanup off that ordinary path; enabled
    // writes retain the original scope through the payload AND notification.
    if (RuntimeGuestSourceCoordinator().BeginGuardedOperation()) {
        runtime_memory_access_detail::StoreTrackedScalar(base, address, value);
        return;
    }
    runtime_memory_access_detail::StoreScalarPayload(base, address, value);
}

// Cold U32 path: MMIO, source tracking enabled, or the tracking mode not yet
// frozen (the first guarded store freezes it exactly as before).
__declspec(noinline) inline void RuntimePpcStoreU32Slow(uint8_t* base, uint32_t address,
                                                        uint32_t value) {
    if (RuntimeIsArchitectedMmioAddress(address)) {
        RuntimeMmioStoreU32(base, address, value);
        return;
    }
    RuntimePpcStoreScalar(base, address, value);
}

// Retain a shared U32 boundary: copying its body into ~230k generated call
// sites would grow code size. Its ordinary path makes only tail calls, so it
// needs no register saves.
__declspec(noinline) inline void RuntimePpcStoreU32(uint8_t* base, uint32_t address,
                                                    uint32_t value) {
    auto* coordinator = runtime_source_memory_detail::initialized_guest_coordinator.load(
        std::memory_order_acquire);
    if (RuntimeIsArchitectedMmioAddress(address) || !coordinator ||
        !coordinator->TrackingSettledDisabled()) {
        RuntimePpcStoreU32Slow(base, address, value);
        return;
    }
    *reinterpret_cast<volatile uint32_t*>(base + address) = __builtin_bswap32(value);
    if (RuntimeGuestStoreMayReachPhysicalMemory(address, 4u) &&
        !RuntimeGuestPhysicalWriteAlreadyPending(address, 4u)) {
        RuntimeNotifyGuestPhysicalWrite(address, 4u, nullptr, 0);
    }
}

#define PPC_LOAD_U32(x) RuntimePpcLoadU32(base, static_cast<uint32_t>(x))

// Generated PPC memory operations are overridden at compile time; generated
// game translation units remain untouched. Ordinary RAM stays a direct load or
// store. Only physical-alias writes notify the GPU cache, and only statically
// tagged MMIO operations consult the runtime MMIO registry.
#define PPC_STORE_U8(x, y)                                                        \
    do {                                                                           \
        const uint32_t ppc_address_ = static_cast<uint32_t>(x);                    \
        const uint8_t ppc_value_ = static_cast<uint8_t>(y);                        \
        if constexpr (kCompletedAllocatorStoreTraceEnabled) {                     \
            if (RuntimeAllocatorHeaderStoreIsSuspicious(ppc_address_, 1,          \
                                                        ppc_value_))              \
                RuntimeObserveAllocatorHeaderStore(base, ppc_address_, 1,         \
                                                   ppc_value_, _ReturnAddress()); \
        }                                                                          \
        RuntimePpcStoreScalar(base, ppc_address_, ppc_value_);                      \
    } while (0)

#define PPC_STORE_U16(x, y)                                                       \
    do {                                                                           \
        const uint32_t ppc_address_ = static_cast<uint32_t>(x);                    \
        const uint16_t ppc_value_ = static_cast<uint16_t>(y);                      \
        if constexpr (kCompletedAllocatorStoreTraceEnabled) {                     \
            if (RuntimeAllocatorHeaderStoreIsSuspicious(ppc_address_, 2,          \
                                                        ppc_value_))              \
                RuntimeObserveAllocatorHeaderStore(base, ppc_address_, 2,         \
                                                   ppc_value_, _ReturnAddress()); \
        }                                                                          \
        RuntimePpcStoreScalar(base, ppc_address_, ppc_value_);                      \
    } while (0)

#define PPC_STORE_U32(x, y)                                                       \
    do {                                                                           \
        const uint32_t ppc_address_ = static_cast<uint32_t>(x);                    \
        const uint32_t ppc_value_ = static_cast<uint32_t>(y);                      \
        if constexpr (kCompletedAllocatorStoreTraceEnabled) {                     \
            if (RuntimeAllocatorHeaderStoreIsSuspicious(ppc_address_, 4,          \
                                                        ppc_value_))              \
                RuntimeObserveAllocatorHeaderStore(base, ppc_address_, 4,         \
                                                   ppc_value_, _ReturnAddress()); \
            if (RuntimeItemSmartPointerStoreIsRelevant(base, ppc_address_,        \
                                                       ppc_value_))               \
                RuntimeObserveItemSmartPointerStore(base, ppc_address_,           \
                                                    ppc_value_, _ReturnAddress()); \
        }                                                                          \
        if (RuntimeGuestPlainStackAddress(ppc_address_, 4u))                        \
            *reinterpret_cast<volatile uint32_t*>(base + ppc_address_) =           \
                __builtin_bswap32(ppc_value_);                                     \
        else                                                                       \
            RuntimePpcStoreU32(base, ppc_address_, ppc_value_);                     \
    } while (0)

#define PPC_STORE_U64(x, y)                                                       \
    do {                                                                           \
        const uint32_t ppc_address_ = static_cast<uint32_t>(x);                    \
        const uint64_t ppc_value_ = static_cast<uint64_t>(y);                      \
        if constexpr (kCompletedAllocatorStoreTraceEnabled) {                     \
            if (RuntimeAllocatorHeaderStoreIsSuspicious(ppc_address_, 8,          \
                                                        ppc_value_))              \
                RuntimeObserveAllocatorHeaderStore(base, ppc_address_, 8,         \
                                                   ppc_value_, _ReturnAddress()); \
        }                                                                          \
        RuntimePpcStoreScalar(base, ppc_address_, ppc_value_);                      \
    } while (0)

// XenonRecomp supplies the vector value already shuffled into guest byte
// order. Route the store through the same explicit physical-memory invalidation
// contract as scalar generated stores; ReXGlue cannot install page-fault
// watches when it adopts the runtime-owned guest mapping.
#define PPC_STORE_V128(x, y)                                                      \
    do {                                                                           \
        const uint32_t ppc_address_ = static_cast<uint32_t>(x);                    \
        const simde__m128i ppc_value_ = (y);                                       \
        if (RuntimeGuestPlainStackAddress(ppc_address_, 16u)) {                    \
            simde_mm_store_si128(reinterpret_cast<simde__m128i*>(base + ppc_address_), \
                                 ppc_value_);                                       \
            break;                                                                 \
        }                                                                          \
        const RuntimeGuestSourceWriteScope source_write_(ppc_address_, 16); \
        simde_mm_store_si128(reinterpret_cast<simde__m128i*>(base + ppc_address_), \
                             ppc_value_);                                           \
        if (RuntimeGuestStoreMayReachPhysicalMemory(ppc_address_, 16u))            \
            RuntimeNotifyGuestPhysicalWriteChecked(ppc_address_, 16u);             \
    } while (0)

#define PPC_MM_LOAD_U8(x) RuntimeMmioLoadU8(base, static_cast<uint32_t>(x))
#define PPC_MM_LOAD_U16(x) RuntimeMmioLoadU16(base, static_cast<uint32_t>(x))
#define PPC_MM_LOAD_U32(x) RuntimeMmioLoadU32(base, static_cast<uint32_t>(x))
#define PPC_MM_LOAD_U64(x) RuntimeMmioLoadU64(base, static_cast<uint32_t>(x))
#define PPC_MM_STORE_U8(x, y) RuntimeMmioStoreU8(base, static_cast<uint32_t>(x), (y))
#define PPC_MM_STORE_U16(x, y) RuntimeMmioStoreU16(base, static_cast<uint32_t>(x), (y))
#define PPC_MM_STORE_U32(x, y) RuntimeMmioStoreU32(base, static_cast<uint32_t>(x), (y))
#define PPC_MM_STORE_U64(x, y) RuntimeMmioStoreU64(base, static_cast<uint32_t>(x), (y))
