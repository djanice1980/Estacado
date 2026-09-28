#pragma once

#include <cstdint>

struct PPCContext;

[[noreturn]] void RuntimeTrap(const char* importName, PPCContext& context, uint8_t* base);
