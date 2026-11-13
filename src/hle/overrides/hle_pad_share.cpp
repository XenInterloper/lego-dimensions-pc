// Fase 80: entrada por jogador com o app LegoToypad aberto. Ver hle_pad_share.h.
//
// Uma thread propria (iniciada em scePadInit):
//   - sem o bloco do app: tenta abri-lo uma vez por segundo, e so;
//   - com o bloco: le a mascara a cada 4 ms (o app escreve a cada 8 ms com o seletor aberto) e confere
//     se o app ainda esta vivo (heartbeat de ate 1 s e o processo do toypadPid). App morto, travado ou
//     bloco invalido = ninguem capturado, e o bloco e fechado ate o app criar outro;
//   - a cada 250 ms, publica o roster (controle de cada jogador) para o app.
// scePadReadState so le os atomicos daqui: nada de chamada ao sistema por leitura de controle.
#include "hle/overrides/hle_pad_share.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <windows.h>

#include "hle/hle_log.h"
#include "hle/overrides/hle_pad_sdl.h"
#include "hle/overrides/hle_avoid_attack.h"
#include "hle/overrides/hle_videoout.h"

namespace hle {
bool PadDevicePresent(int slot); // hle_pad.cpp
int PadKeyboardSlot();
}

namespace hle::padshare {

namespace {

constexpr wchar_t kOwnershipName[] = L"Local\\LegoToypadInputOwnership";
constexpr wchar_t kRosterName[] = L"Local\\LegoDimensionsInputRoster";
constexpr ULONGLONG kStaleMs = 1000;
constexpr ULONGLONG kReopenMs = 1000;
constexpr ULONGLONG kRosterMs = 250;
constexpr u32 kPlayerMask = (1u << padsdl::kSlots) - 1;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_started{false};
std::atomic<u32> g_captured{0};
std::atomic<bool> g_picker_front{false};
std::atomic<bool> g_keyboard_shared{false};

// Copia coerente de um bloco com seqlock. false se o escritor nao terminou em 64 tentativas (ou seja,
// travou no meio de uma escrita): quem chama mantem o valor anterior.
template <typename T>
bool SeqlockRead(const void* shared, T* out) {
    const volatile u32* sequence = reinterpret_cast<const volatile u32*>(static_cast<const u8*>(shared) + offsetof(T, sequence));
    for (int attempt = 0; attempt < 64; ++attempt) {
        const u32 before = *sequence;
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        std::memcpy(out, shared, sizeof(T));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (*sequence == before) {
            return true;
        }
    }
    return false;
}

template <typename T>
void SeqlockWrite(void* shared, const T& value) {
    volatile u32* sequence = reinterpret_cast<volatile u32*>(static_cast<u8*>(shared) + offsetof(T, sequence));
    const u32 writing = *sequence | 1; // impar: escrevendo (par no fim, mesmo se o bloco veio com lixo)
    *sequence = writing;
    std::atomic_thread_fence(std::memory_order_release);
    T copy = value;
    copy.sequence = writing;
    std::memcpy(shared, &copy, sizeof(T));
    std::atomic_thread_fence(std::memory_order_release);
    *sequence = writing + 1;
}

// ---- Local\LegoToypadInputOwnership ----------------------------------------------------------------
struct Ownership {
    HANDLE mapping = nullptr;
    const void* view = nullptr;
    ULONGLONG next_open = 0;
    u32 pid = 0;
    HANDLE process = nullptr;
    bool live = false;
    ToypadInputOwnership last{};
};
Ownership g_own;

void CloseOwnership() {
    if (g_own.view) {
        UnmapViewOfFile(g_own.view);
    }
    if (g_own.mapping) {
        CloseHandle(g_own.mapping);
    }
    if (g_own.process) {
        CloseHandle(g_own.process);
    }
    g_own.view = nullptr;
    g_own.mapping = nullptr;
    g_own.process = nullptr;
    g_own.pid = 0;
}

void SetLive(bool live, const char* why) {
    if (live == g_own.live) {
        return;
    }
    g_own.live = live;
    if (live) {
        log::Tagged("TOYPAD-INPUT", "LegoToypad per-player input block found (pid %u)", g_own.pid);
    } else {
        log::Tagged("TOYPAD-INPUT", "LegoToypad per-player input block released (%s): every player back to normal", why);
    }
}

// Processo do app ainda existe? Um handle por pid, aberto uma vez.
bool ProcessAlive(u32 pid) {
    if (pid != g_own.pid) {
        if (g_own.process) {
            CloseHandle(g_own.process);
        }
        g_own.pid = pid;
        g_own.process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    }
    return g_own.process && WaitForSingleObject(g_own.process, 0) == WAIT_TIMEOUT;
}

// Uma passada: atualiza g_captured, g_picker_front e g_keyboard_shared. Devolve true se o bloco esta vivo.
bool PollOwnership(ULONGLONG now) {
    auto release = [](const char* why) {
        g_captured = 0;
        g_picker_front = false;
        g_keyboard_shared = false;
        SetLive(false, why);
    };
    if (!g_own.view) {
        if (now < g_own.next_open) {
            return false;
        }
        g_own.next_open = now + kReopenMs;
        g_own.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kOwnershipName);
        if (!g_own.mapping) {
            return false;
        }
        g_own.view = MapViewOfFile(g_own.mapping, FILE_MAP_READ, 0, 0, sizeof(ToypadInputOwnership));
        if (!g_own.view) {
            CloseHandle(g_own.mapping);
            g_own.mapping = nullptr;
            return false;
        }
    }
    ToypadInputOwnership block;
    if (!SeqlockRead(g_own.view, &block)) {
        block = g_own.last; // escritor no meio de uma escrita: fica com a copia anterior (o heartbeat ainda vale)
    }
    g_own.last = block;
    const char* dead = nullptr;
    if (block.magic != kOwnershipMagic || block.version != kOwnershipVersion) {
        dead = "invalid magic / version";
    } else if (now > block.heartbeatTickMs && now - block.heartbeatTickMs > kStaleMs) {
        dead = "heartbeat older than 1 s";
    } else if (!ProcessAlive(block.toypadPid)) {
        dead = "LegoToypad process is gone";
    }
    if (dead) {
        release(dead);
        CloseOwnership(); // o app cria outro bloco quando voltar; tenta de novo em 1 s
        g_own.next_open = now + kReopenMs;
        return false;
    }
    SetLive(true, "");
    DWORD front_pid = 0;
    if (const HWND front = GetForegroundWindow()) {
        GetWindowThreadProcessId(front, &front_pid);
    }
    const u32 captured = block.capturedPlayers & kPlayerMask;
    const bool in_front = front_pid != 0 && front_pid == block.toypadPid;
    if (g_captured.exchange(captured) != captured) {
        log::Tagged("TOYPAD-INPUT", "captured players: P1 %s, P2 %s, P3 %s, P4 %s (flags 0x%x)", (captured & 1) ? "yes" : "no",
                    (captured & 2) ? "yes" : "no", (captured & 4) ? "yes" : "no", (captured & 8) ? "yes" : "no", block.flags);
    }
    if (in_front && !g_picker_front.exchange(true)) {
        videoout::OnToypadInFront(); // o painel abriu: o ponteiro vai para o meio dele
    }
    g_picker_front = in_front;
    g_keyboard_shared = (block.flags & kOwnershipFlagShareKeyboard) != 0;
    return true;
}

// ---- Local\LegoDimensionsInputRoster -----------------------------------------------------------------
HANDLE g_roster_mapping = nullptr;
void* g_roster_view = nullptr;

// Fase 82: zonas com personagens presos por um ataque de chefe (hle_avoid_attack: o estado do proprio jogo, lido
// por ganchos no eboot). LD_PAD_FREEZE=<mascara> forca um valor (teste sem o chefe); LD_PAD_FREEZE_DETECT=0 desliga
// (o bit kRosterCapPadFreeze some e o LegoToypad ignora o atalho). Sem os ganchos (outro eboot) o bit tambem some.
int ForcedFreeze() { // -1 = sem forcar
    static const int forced = [] {
        const char* value = std::getenv("LD_PAD_FREEZE");
        return value && *value ? static_cast<int>(std::strtoul(value, nullptr, 0) & 7) : -1;
    }();
    return forced;
}

bool FreezeDetection() {
    static const bool allowed = [] {
        const char* value = std::getenv("LD_PAD_FREEZE_DETECT");
        return !(value && *value == '0');
    }();
    return ForcedFreeze() >= 0 || (allowed && avoidattack::Installed());
}

uint32_t CurrentFrozenZones() {
    if (ForcedFreeze() >= 0) {
        return static_cast<uint32_t>(ForcedFreeze());
    }
    return FreezeDetection() ? avoidattack::FrozenZones() : 0;
}

std::atomic<uint32_t> g_frozen_published{0};

void PublishRoster(ULONGLONG now) {
    if (!g_roster_view) {
        if (!g_roster_mapping) {
            g_roster_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GameInputRoster), kRosterName);
            if (!g_roster_mapping) {
                log::Tagged("TOYPAD-INPUT", "could not create the input roster block (error %lu)", GetLastError());
                return;
            }
        }
        g_roster_view = MapViewOfFile(g_roster_mapping, FILE_MAP_WRITE, 0, 0, sizeof(GameInputRoster));
        if (!g_roster_view) {
            return;
        }
    }
    GameInputRoster roster{};
    roster.magic = kRosterMagic;
    roster.version = kRosterVersion;
    roster.gamePid = GetCurrentProcessId();
    roster.capabilities = kRosterCapOwnership | kRosterCapShareKeyboard | (FreezeDetection() ? kRosterCapPadFreeze : 0);
    roster.frozenZones = g_frozen_published.load();
    roster.heartbeatTickMs = now;
    roster.appliedCapture = g_captured.load();
    const int sdl_count = padsdl::Count();
    const int keyboard_slot = PadKeyboardSlot();
    for (int i = 0; i < padsdl::kSlots; ++i) {
        GameInputSlot& slot = roster.slot[i];
        slot.sdlInstanceId = -1;
        slot.xinputIndex = -1;
        padsdl::Device device;
        if (padsdl::GetDevice(i, &device)) {
            slot.connected = 1;
            slot.kind = kSlotKindController;
            slot.flags = kSlotFlagSdl;
            slot.sdlInstanceId = device.instance_id;
            std::memcpy(slot.joystickGuid, device.guid, sizeof(slot.joystickGuid));
            slot.vendorId = device.vendor;
            slot.productId = device.product;
            std::snprintf(slot.devicePath, sizeof(slot.devicePath), "%s", device.path);
            std::snprintf(slot.name, sizeof(slot.name), "%s", device.name);
            if (std::strncmp(device.path, "XInput#", 7) == 0) {
                slot.xinputIndex = std::atoi(device.path + 7);
            }
        } else if (PadDevicePresent(i)) {
            slot.connected = 1;
            // sem controle no SDL e o XInput do mesmo indice (hle_pad.cpp DevicePresent); senao, controle de teste
            const bool xinput = sdl_count == 0 && !(i == 1 && std::getenv("LD_PAD_LIVE2"));
            slot.kind = xinput ? kSlotKindController : kSlotKindVirtual;
            if (xinput) {
                slot.flags = kSlotFlagXInput;
                slot.xinputIndex = i;
                std::snprintf(slot.devicePath, sizeof(slot.devicePath), "XInput#%d", i);
            }
        }
        if (i == 0) { // o jogador 1 sempre existe
            slot.connected = 1;
        }
        if (i == keyboard_slot) { // o teclado e do jogador 1 ou do 2 (Fase 81: launcher.ini ou F3)
            slot.connected = 1;
            slot.flags |= kSlotFlagKeyboard;
        }
        if ((slot.flags & kSlotFlagKeyboard) && slot.kind == kSlotKindNone) {
            slot.kind = kSlotKindKeyboard; // so o teclado (o jogador 1 sem controle nem teclado fica kind 0, conectado)
        }
        roster.playerCount += slot.connected;
    }
    SeqlockWrite(g_roster_view, roster);
}

void Thread() {
    log::Tagged("TOYPAD-INPUT", "per-player input with LegoToypad: reading Local\\LegoToypadInputOwnership, "
                                "publishing Local\\LegoDimensionsInputRoster");
    ULONGLONG next_roster = 0;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const bool live = PollOwnership(now);
        const uint32_t frozen = CurrentFrozenZones();
        const uint32_t before = g_frozen_published.exchange(frozen);
        if (frozen != before) { // Fase 82: na hora, fora do ciclo
            log::Tagged("TOYPAD-INPUT", "frozen toy pad zones: 0x%x (bit 0 = centre, 1 = left, 2 = right)", frozen);
        }
        if (now >= next_roster || frozen != before) {
            next_roster = now + kRosterMs;
            PublishRoster(now);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(live ? 4 : 50));
    }
}

} // namespace

void SetEnabled(bool enabled) {
    g_enabled = enabled;
}

void Start() {
    if (!g_enabled || g_started.exchange(true)) {
        return;
    }
    std::thread(Thread).detach();
}

uint32_t CapturedPlayers() {
    return g_captured.load(std::memory_order_relaxed);
}

bool PickerInFront() {
    return g_picker_front.load(std::memory_order_relaxed);
}

bool ShareKeyboardRequested() {
    return g_keyboard_shared.load(std::memory_order_relaxed);
}

} // namespace hle::padshare
