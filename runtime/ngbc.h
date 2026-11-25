// ngbc.h - interface comum do runtime do port NGBC
#pragma once
#include "ppc_recomp_shared.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

extern uint8_t* g_base;          // base da memoria do convidado (4 GB reservados)
extern std::string g_data_root;  // pasta com os arquivos do jogo (data/, media/)

inline uint8_t rd8(uint32_t a) { return g_base[a]; }
inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
inline void wr8(uint32_t a, uint8_t v) { g_base[a] = v; }
inline void wr16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
inline void wr32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
inline void wr64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }

void commit(uint32_t addr, uint64_t size);
uint32_t guest_alloc(uint64_t size, uint64_t align = 0x1000);
uint32_t guest_alloc_phys(uint64_t size, uint64_t align);
uint32_t guest_alloc_stack(uint64_t size);

void trace_import(const char* name, PPCContext& ctx);
void ngbc_print_summary(const char* motivo);

// kernel.cpp
void kernel_spawn_main(void (*fn)(PPCContext&, uint8_t*));
void kernel_spawn_thread(std::function<void(PPCContext&, uint8_t*)> body);   // thread do convidado sem handle