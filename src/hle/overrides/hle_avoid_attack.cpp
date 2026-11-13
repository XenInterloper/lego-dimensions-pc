// Fase 82: ver hle_avoid_attack.h.
//
// Engenharia reversa (eboot 1.24), para quem precisar conferir ou portar para outra versao:
//   AvoidAttack (classe base, 0x380 bytes; vtable eboot+0x2b344d0; ~25 chefes: Sauron, Zod, Davros, Gozer, Vortech,
//   Mercado, Obscurus, aranha mecanica...). Atualizacao por quadro = slot 107 da vtable, eboot+0x301c00
//   (this em rdi, dt em xmm0, devolve bool). Unica subclasse que troca o slot 107 so salta para ele (eboot+0x1d43b20).
//     +0x16c  s32  zona atacada (0 centro, 1 esquerda, 2 direita; -1 = nenhuma). Escolhida por eboot+0x3024e0, que
//                  conta os personagens de cada zona (eboot+0x1581db0) e agarra ate 3 da zona com mais.
//     +0x170, +0x188, +0x1a0  vagas de vitima (ponteiro fraco: personagem em +0, lista em +8 / +0x10). A cada quadro,
//                  a vitima cuja figura (info em [personagem+0x370] -> +0x1f8 -> +0x28) esta noutra zona e solta e a
//                  vaga zera; as outras continuam presas (slot 0x3e8 da vtable).
//     +0x2cc  u16  flags (0x40 = ninguem mais preso).
//   Bruxa Ma do Oeste (fase 1A, vtable eboot+0x2b23ef0): a mesma logica copiada, atualizacao eboot+0x270da0.
//     +0xdc   s32  zona atacada (escolhida por eboot+0x271b00);  +0x268, +0x280, +0x298  vagas de vitima.
//   Numeracao das zonas: o HUD do aviso (eboot+0x315d00) acende AVOIDATTACK_MIDDLE com 0, _LEFT com 1 e _RIGHT com 2,
//   e o driver do portal guarda os LEDs por zona fisica - 1 (1 centro, 2 esquerda, 3 direita): zona do jogo n = bit n.
#include "hle/overrides/hle_avoid_attack.h"

#include <windows.h>

#include <Zydis/Zydis.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "hle/generated/hle_stub.h"
#include "hle/hle_log.h"

namespace hle::avoidattack {

namespace {

struct Layout {
    const char* name;
    u64 update;      // offset da funcao de atualizacao no eboot
    u8 prologue[17]; // bytes esperados no comeco dela (conferidos antes de por o gancho)
    u32 dock;        // s32: zona atacada
    u32 victims[3];  // ponteiros das vitimas presas
};

constexpr Layout kBase{"AvoidAttack",
                       0x301c00,
                       {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x68},
                       0x16c,
                       {0x170, 0x188, 0x1a0}};
constexpr Layout kWitch{"WickedWitch",
                        0x270da0,
                        {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xd8},
                        0xdc,
                        {0x268, 0x280, 0x298}};

// Instancias vistas recentemente. O estado e lido DENTRO da atualizacao do jogo (o objeto existe naquela hora) e
// guardado aqui ja resumido; depois disso o ponteiro nao e mais lido, so comparado. Uma instancia que para de ser
// atualizada (destruida, ou o chefe acabou) sai depois de kStaleMs.
constexpr ULONGLONG kStaleMs = 400;
struct Seen {
    u64 self = 0;
    u8 mask = 0;
    ULONGLONG tick = 0;
};
std::mutex g_mutex;
std::array<Seen, 16> g_seen{};
std::atomic<bool> g_installed{false};

u8 ReadMask(const u8* self, const Layout& layout) {
    s32 dock;
    std::memcpy(&dock, self + layout.dock, sizeof(dock));
    if (dock < 0 || dock > 2) {
        return 0;
    }
    for (const u32 offset : layout.victims) {
        u64 victim;
        std::memcpy(&victim, self + offset, sizeof(victim));
        if (victim) {
            return static_cast<u8>(1u << dock); // alguem continua preso nessa zona
        }
    }
    return 0;
}

void Record(const void* self, u8 mask) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    Seen* slot = nullptr;
    for (Seen& seen : g_seen) {
        if (seen.self == reinterpret_cast<u64>(self)) {
            slot = &seen;
            break;
        }
        if (!slot && (seen.self == 0 || now - seen.tick > kStaleMs)) {
            slot = &seen;
        }
    }
    if (!slot) {
        return; // mais de 16 ataques vivos ao mesmo tempo: nao acontece no jogo
    }
    if (slot->self != reinterpret_cast<u64>(self) || slot->mask != mask) {
        log::Tagged("AVOID", "attack 0x%llx: frozen zones 0x%x", static_cast<unsigned long long>(reinterpret_cast<u64>(self)), mask);
    }
    slot->self = reinterpret_cast<u64>(self);
    slot->mask = mask;
    slot->tick = now;
}

// Assinatura das duas atualizacoes: this em rdi, dt em xmm0, bool em al. Os outros registradores de argumento sao
// repassados como vieram (nenhuma das duas os usa, mas assim o gancho nao muda nada do que o jogo passa).
using UpdateFn = bool(HLE_SYSV_ABI*)(void*, u64, u64, u64, u64, u64, float);
UpdateFn g_base_update = nullptr;
UpdateFn g_witch_update = nullptr;

HLE_SYSV_ABI bool BaseUpdate(void* self, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, float dt) {
    const bool result = g_base_update(self, a1, a2, a3, a4, a5, dt);
    Record(self, ReadMask(static_cast<const u8*>(self), kBase));
    return result;
}

HLE_SYSV_ABI bool WitchUpdate(void* self, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, float dt) {
    const bool result = g_witch_update(self, a1, a2, a3, a4, a5, dt);
    Record(self, ReadMask(static_cast<const u8*>(self), kWitch));
    return result;
}

// Mesmo gancho do hle_anomaly: as instrucoes inteiras do comeco (>= 14 bytes, sem enderecos relativos) vao para um
// trampolim que volta ao resto da funcao, e no lugar fica um "jmp [rip+0]" para o gancho.
void WriteJump(u8* at, const void* to) {
    at[0] = 0xFF;
    at[1] = 0x25;
    std::memset(at + 2, 0, 4);
    const u64 target = reinterpret_cast<u64>(to);
    std::memcpy(at + 6, &target, 8);
}

void* Hook(u8* base, const Layout& layout, const void* replacement) {
    u8* target = base + layout.update;
    if (std::memcmp(target, layout.prologue, sizeof(layout.prologue)) != 0) {
        log::Tagged("AVOID", "eboot+0x%llx (%s): different code, boss attack detection off", static_cast<unsigned long long>(layout.update),
                    layout.name);
        return nullptr;
    }
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t length = 0;
    while (length < 14) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, target + length, 32, &insn, ops)) ||
            (insn.attributes & ZYDIS_ATTRIB_IS_RELATIVE)) {
            return nullptr;
        }
        length += insn.length;
    }
    auto* trampoline = static_cast<u8*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return nullptr;
    }
    std::memcpy(trampoline, target, length);
    WriteJump(trampoline + length, target + length);
    DWORD old = 0;
    VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &old);
    WriteJump(target, replacement);
    std::memset(target + 14, 0x90, length - 14);
    VirtualProtect(target, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return trampoline;
}

} // namespace

bool Install(u8* base) {
    // so o eboot 1.24 (mesma marca que o hle_anomaly confere)
    const char* marker = reinterpret_cast<const char*>(base + 0x2421EBD);
    const size_t len = strnlen(marker, 256);
    if (len < 9 || _strnicmp(marker + len - 9, "_PATCH11C", 9) != 0) {
        log::Tagged("AVOID", "eboot is not version 1.24: boss attack detection off");
        return false;
    }
    g_base_update = reinterpret_cast<UpdateFn>(Hook(base, kBase, reinterpret_cast<const void*>(&BaseUpdate)));
    g_witch_update = reinterpret_cast<UpdateFn>(Hook(base, kWitch, reinterpret_cast<const void*>(&WitchUpdate)));
    g_installed = g_base_update != nullptr; // a Bruxa e um extra: sem ela o resto vale
    log::Tagged("AVOID", "boss attack detection: %s%s", g_installed ? "on" : "off",
                g_installed && !g_witch_update ? " (without the Wicked Witch)" : "");
    return g_installed;
}

bool Installed() {
    return g_installed.load();
}

u32 FrozenZones() {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    u32 mask = 0;
    for (const Seen& seen : g_seen) {
        if (seen.self && now - seen.tick <= kStaleMs) {
            mask |= seen.mask;
        }
    }
    return mask;
}

} // namespace hle::avoidattack
