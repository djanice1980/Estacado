#pragma once

#include <cstdint>

using RuntimeMmioRead32 = uint32_t (*)(void* context, uint32_t address);
using RuntimeMmioWrite32 = void (*)(void* context, uint32_t address, uint32_t value);

bool RegisterRuntimeMmioRange(uint32_t first, uint32_t last, void* context,
                              RuntimeMmioRead32 read, RuntimeMmioWrite32 write);
void ResetRuntimeMmioRangesForTests();
bool IsRuntimeMmioAddress(uint32_t address);

uint8_t RuntimeMmioLoadU8(uint8_t* base, uint32_t address);
uint16_t RuntimeMmioLoadU16(uint8_t* base, uint32_t address);
uint32_t RuntimeMmioLoadU32(uint8_t* base, uint32_t address);
uint64_t RuntimeMmioLoadU64(uint8_t* base, uint32_t address);
void RuntimeMmioStoreU8(uint8_t* base, uint32_t address, uint8_t value);
void RuntimeMmioStoreU16(uint8_t* base, uint32_t address, uint16_t value);
void RuntimeMmioStoreU32(uint8_t* base, uint32_t address, uint32_t value);
void RuntimeMmioStoreU64(uint8_t* base, uint32_t address, uint64_t value);
