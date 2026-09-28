#include "runtime_source_memory_coordinator.h"
#include "runtime_diagnostics.h"

#include "runtime_graphics.h"
#include "ppc_recomp_shared.h"
#include "runtime_threads.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
struct ActivePpcContext {
    PPCContext* context{};
    uint8_t* base{};
    uint32_t guestFunction{};
};

thread_local ActivePpcContext active{};
constexpr uint64_t kHighestMappedGuestExclusive = 0xFFD00000ull;

void Append(char* buffer, size_t bufferSize, const char* format, ...) {
    const size_t used = std::strlen(buffer);
    if (used >= bufferSize) return;
    va_list args;
    va_start(args, format);
    _vsnprintf_s(buffer + used, bufferSize - used, _TRUNCATE, format, args);
    va_end(args);
}

bool IsMapped(uint32_t address, uint32_t size) {
    return uint64_t(address) + size <= kHighestMappedGuestExclusive;
}

[[noreturn]] void FailGuestAccess(const char* operation, uint32_t address, uint32_t size, void* caller) {
    char message[512]{};
    _snprintf_s(message, sizeof(message), _TRUNCATE,
        "PPC_MEMORY_FAULT thread=%u function=0x%08x operation=%s guest=0x%08x size=%u host_caller=0x%p",
        CurrentGuestThreadId(), active.guestFunction, operation, address, size, caller);
    std::cerr << message << '\n';
    throw std::runtime_error(message);
}

template <typename T>
T Load(uint8_t* base, uint32_t address, void* caller) {
    if (!IsMapped(address, sizeof(T))) FailGuestAccess("read", address, sizeof(T), caller);
    T value{};
    std::memcpy(&value, base + address, sizeof(value));
    if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
    if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
    if constexpr (sizeof(T) == 8) return __builtin_bswap64(value);
    return value;
}

template <typename T>
void Store(uint8_t* base, uint32_t address, T value, void* caller) {
    if (!IsMapped(address, sizeof(T))) FailGuestAccess("write", address, sizeof(T), caller);
    if constexpr (sizeof(T) == 2) value = __builtin_bswap16(value);
    if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
    if constexpr (sizeof(T) == 8) value = __builtin_bswap64(value);
    const RuntimeGuestSourceWriteScope source_write(address, sizeof(value));
    std::memcpy(base + address, &value, sizeof(value));
    RuntimeNotifyGuestPhysicalWrite(address, sizeof(T), __FILE__, __LINE__);
}

void AppendRegisterState(char* buffer, size_t bufferSize, const PPCContext& context) {
    const uint64_t registers[] = {
        context.r0.u64, context.r1.u64, context.r2.u64, context.r3.u64,
        context.r4.u64, context.r5.u64, context.r6.u64, context.r7.u64,
        context.r8.u64, context.r9.u64, context.r10.u64, context.r11.u64,
        context.r12.u64, context.r13.u64, context.r14.u64, context.r15.u64,
        context.r16.u64, context.r17.u64, context.r18.u64, context.r19.u64,
        context.r20.u64, context.r21.u64, context.r22.u64, context.r23.u64,
        context.r24.u64, context.r25.u64, context.r26.u64, context.r27.u64,
        context.r28.u64, context.r29.u64, context.r30.u64, context.r31.u64,
    };
    for (uint32_t index = 0; index < 32; ++index) {
        Append(buffer, bufferSize, "r%u=0x%llx%s", index,
            static_cast<unsigned long long>(registers[index]), index % 4 == 3 ? "\n" : " ");
    }
    Append(buffer, bufferSize, "lr=0x%llx ctr=0x%llx xer={so=%u ov=%u ca=%u}\n",
        static_cast<unsigned long long>(context.lr), static_cast<unsigned long long>(context.ctr.u64),
        context.xer.so, context.xer.ov, context.xer.ca);
    const PPCCRRegister conditions[] = {context.cr0, context.cr1, context.cr2, context.cr3,
        context.cr4, context.cr5, context.cr6, context.cr7};
    for (uint32_t index = 0; index < 8; ++index) {
        Append(buffer, bufferSize, "cr%u={lt=%u gt=%u eq=%u so=%u}%s", index,
            conditions[index].lt, conditions[index].gt, conditions[index].eq, conditions[index].so,
            index == 7 ? "\n" : " ");
    }
}
}

void RuntimeSetActivePpcContext(PPCContext* context, uint8_t* base, uint32_t guestFunction) {
    active = {context, base, guestFunction};
}

PPCContext* RuntimeActivePpcContext() { return active.context; }

uint32_t RuntimeActiveGuestFunction() { return active.guestFunction; }

uint32_t RuntimeActiveGuestLr() { return active.context ? active.context->lr : 0; }

void RuntimeAppendPpcCrashEvidence(char* buffer, size_t bufferSize) {
    if (!active.context) return;
    Append(buffer, bufferSize, "PPC_ACTIVE thread=%u function=0x%08x base=0x%p\n",
        CurrentGuestThreadId(), active.guestFunction, active.base);
    AppendRegisterState(buffer, bufferSize, *active.context);
}

uint8_t RuntimeDiagnosticLoadU8(uint8_t* base, uint32_t address, void* caller) { return Load<uint8_t>(base, address, caller); }
uint16_t RuntimeDiagnosticLoadU16(uint8_t* base, uint32_t address, void* caller) { return Load<uint16_t>(base, address, caller); }
uint32_t RuntimeDiagnosticLoadU32(uint8_t* base, uint32_t address, void* caller) { return Load<uint32_t>(base, address, caller); }
uint64_t RuntimeDiagnosticLoadU64(uint8_t* base, uint32_t address, void* caller) { return Load<uint64_t>(base, address, caller); }
void RuntimeDiagnosticStoreU8(uint8_t* base, uint32_t address, uint8_t value, void* caller) { Store<uint8_t>(base, address, value, caller); }
void RuntimeDiagnosticStoreU16(uint8_t* base, uint32_t address, uint16_t value, void* caller) { Store<uint16_t>(base, address, value, caller); }
void RuntimeDiagnosticStoreU32(uint8_t* base, uint32_t address, uint32_t value, void* caller) { Store<uint32_t>(base, address, value, caller); }
void RuntimeDiagnosticStoreU64(uint8_t* base, uint32_t address, uint64_t value, void* caller) { Store<uint64_t>(base, address, value, caller); }
