#pragma once

#include <cstdint>
#include <intrin.h>

struct PPCContext;

void RuntimeSetActivePpcContext(PPCContext* context, uint8_t* base, uint32_t guestFunction);
PPCContext* RuntimeActivePpcContext();
uint32_t RuntimeActiveGuestFunction();
uint32_t RuntimeActiveGuestLr();
void RuntimeAppendPpcCrashEvidence(char* buffer, size_t bufferSize);
uint8_t RuntimeDiagnosticLoadU8(uint8_t* base, uint32_t address, void* caller);
uint16_t RuntimeDiagnosticLoadU16(uint8_t* base, uint32_t address, void* caller);
uint32_t RuntimeDiagnosticLoadU32(uint8_t* base, uint32_t address, void* caller);
uint64_t RuntimeDiagnosticLoadU64(uint8_t* base, uint32_t address, void* caller);
void RuntimeDiagnosticStoreU8(uint8_t* base, uint32_t address, uint8_t value, void* caller);
void RuntimeDiagnosticStoreU16(uint8_t* base, uint32_t address, uint16_t value, void* caller);
void RuntimeDiagnosticStoreU32(uint8_t* base, uint32_t address, uint32_t value, void* caller);
void RuntimeDiagnosticStoreU64(uint8_t* base, uint32_t address, uint64_t value, void* caller);

// Force-included only for ppc_recomp.14.cpp while diagnosing the content
// startup fault. This preserves PPC endian semantics and rejects an unmapped
// access before the host dereference, recording the exact guest address.
#define PPC_LOAD_U8(x) RuntimeDiagnosticLoadU8(base, static_cast<uint32_t>(x), _ReturnAddress())
#define PPC_LOAD_U16(x) RuntimeDiagnosticLoadU16(base, static_cast<uint32_t>(x), _ReturnAddress())
#define PPC_LOAD_U32(x) RuntimeDiagnosticLoadU32(base, static_cast<uint32_t>(x), _ReturnAddress())
#define PPC_LOAD_U64(x) RuntimeDiagnosticLoadU64(base, static_cast<uint32_t>(x), _ReturnAddress())
#define PPC_STORE_U8(x, y) RuntimeDiagnosticStoreU8(base, static_cast<uint32_t>(x), y, _ReturnAddress())
#define PPC_STORE_U16(x, y) RuntimeDiagnosticStoreU16(base, static_cast<uint32_t>(x), y, _ReturnAddress())
#define PPC_STORE_U32(x, y) RuntimeDiagnosticStoreU32(base, static_cast<uint32_t>(x), y, _ReturnAddress())
#define PPC_STORE_U64(x, y) RuntimeDiagnosticStoreU64(base, static_cast<uint32_t>(x), y, _ReturnAddress())
