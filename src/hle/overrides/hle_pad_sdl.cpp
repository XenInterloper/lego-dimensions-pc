// Controle pelo SDL3 (Fase 24.5). Ver hle_pad_sdl.h.
//
// O SDL enxerga o DualShock 4 direto (HIDAPI), sem DS4Windows nem driver de XInput, e entrega os
// botoes pela posicao fisica (SOUTH = cruz). Aqui roda numa thread propria: o SDL e iniciado so
// com o subsistema de gamepad, sem janela, e o estado e lido a cada 4 ms.
#include "hle/overrides/hle_pad_sdl.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

namespace hle::padsdl {

namespace {

constexpr u32 kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80;
constexpr u32 kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800;
constexpr u32 kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000, kTouchPad = 0x100000;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_started{false};
std::mutex g_mutex;
// Fase 61: uma vaga por jogador (co-op local)
struct Slot {
    SDL_Gamepad* pad = nullptr; // so a thread do SDL mexe
    SDL_JoystickID id = 0;
    State state;      // com g_mutex
    char name[96] = ""; // com g_mutex
    const char* icons = ""; // Fase 62: com g_mutex
    Device device;          // Fase 80: com g_mutex
    std::atomic<u32> rumble{0xFFFFFFFF}; // pedido pendente: (grande << 8) | pequeno
};
Slot g_slots[kSlots];

// Eixo do SDL (-32768..32767, +Y para baixo) -> eixo do Orbis (0..255, 128 = centro), com zona morta.
u8 Axis(Sint16 value) {
    int v = value;
    if (v > -7849 && v < 7849) {
        v = 0;
    }
    return static_cast<u8>((v + 32768) >> 8);
}

u8 Trigger(Sint16 value) { // 0..32767 -> 0..255
    return static_cast<u8>(value <= 0 ? 0 : (value >> 7));
}

void Poll(Slot& slot) {
    SDL_Gamepad* const pad = slot.pad;
    State s;
    s.connected = true;
    const struct {
        SDL_GamepadButton sdl;
        u32 orbis;
    } map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, kCross},          {SDL_GAMEPAD_BUTTON_EAST, kCircle},
        {SDL_GAMEPAD_BUTTON_WEST, kSquare},          {SDL_GAMEPAD_BUTTON_NORTH, kTriangle},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL1},     {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR1},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, kL3},        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kR3},
        {SDL_GAMEPAD_BUTTON_START, kOptions},        {SDL_GAMEPAD_BUTTON_TOUCHPAD, kTouchPad},
        {SDL_GAMEPAD_BUTTON_BACK, kTouchPad}, // Share / Back: o jogo nao tem botao para isso; vale como clique do touchpad
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},           {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},       {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},
    };
    for (const auto& m : map) {
        if (SDL_GetGamepadButton(pad, m.sdl)) {
            s.buttons |= m.orbis;
        }
    }
    s.lx = Axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    s.ly = Axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    s.rx = Axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
    s.ry = Axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
    s.l2 = Trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    s.r2 = Trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    if (s.l2 > 30) {
        s.buttons |= kL2;
    }
    if (s.r2 > 30) {
        s.buttons |= kR2;
    }
    // Touchpad: ate dois dedos, na resolucao do DualShock 4 (1920 x 942).
    if (SDL_GetNumGamepadTouchpads(pad) > 0) {
        const int fingers = SDL_GetNumGamepadTouchpadFingers(pad, 0);
        for (int f = 0; f < fingers && s.touches < 2; ++f) {
            bool down = false;
            float x = 0.0f, y = 0.0f, pressure = 0.0f;
            if (SDL_GetGamepadTouchpadFinger(pad, 0, f, &down, &x, &y, &pressure) && down) {
                auto& touch = s.touch[s.touches++];
                touch.x = static_cast<u16>(SDL_clamp(x, 0.0f, 1.0f) * 1919.0f);
                touch.y = static_cast<u16>(SDL_clamp(y, 0.0f, 1.0f) * 941.0f);
                touch.id = static_cast<u8>(f);
            }
        }
    }
    std::lock_guard lock(g_mutex);
    slot.state = s;
}

void Thread() {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); // a janela do jogo nao e do SDL
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::printf("[PAD-SDL] SDL_Init failed: %s (XInput remains available)\n", SDL_GetError());
        return;
    }
    std::printf("[PAD-SDL] SDL %d.%d.%d started; waiting for controllers (up to %d, one per player)\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                SDL_MICRO_VERSION, kSlots);
    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                int free_slot = -1;
                bool known = false;
                for (int i = 0; i < kSlots; ++i) {
                    known = known || (g_slots[i].pad && g_slots[i].id == event.gdevice.which);
                    if (!g_slots[i].pad && free_slot < 0) {
                        free_slot = i;
                    }
                }
                if (known || free_slot < 0) {
                    continue;
                }
                Slot& slot = g_slots[free_slot];
                slot.pad = SDL_OpenGamepad(event.gdevice.which);
                if (slot.pad) {
                    slot.id = event.gdevice.which;
                    const char* name = SDL_GetGamepadName(slot.pad);
                    {
                        std::lock_guard lock(g_mutex);
                        std::snprintf(slot.name, sizeof(slot.name), "%s", name ? name : "controller");
                        // Fase 62: icones dos botoes pelo tipo de controle (os genericos seguem o layout do Xbox)
                        switch (SDL_GetGamepadType(slot.pad)) {
                        case SDL_GAMEPAD_TYPE_PS4:
                        case SDL_GAMEPAD_TYPE_PS5: slot.icons = "ps4"; break;
                        case SDL_GAMEPAD_TYPE_PS3: slot.icons = "ps3"; break;
                        case SDL_GAMEPAD_TYPE_XBOX360: slot.icons = "360"; break;
                        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
                        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
                        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                        case SDL_GAMEPAD_TYPE_GAMECUBE: slot.icons = "wiiu"; break;
                        default: slot.icons = "xboxone"; break;
                        }
                        // Fase 80: para o roster do LegoToypad saber que aparelho e deste jogador
                        Device& device = slot.device;
                        device = Device{};
                        device.instance_id = static_cast<s32>(event.gdevice.which);
                        const SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(slot.pad));
                        std::memcpy(device.guid, guid.data, sizeof(device.guid));
                        device.vendor = SDL_GetGamepadVendor(slot.pad);
                        device.product = SDL_GetGamepadProduct(slot.pad);
                        const char* path = SDL_GetGamepadPath(slot.pad);
                        std::snprintf(device.path, sizeof(device.path), "%s", path ? path : "");
                        std::snprintf(device.name, sizeof(device.name), "%s", slot.name);
                    }
                    SDL_SetGamepadPlayerIndex(slot.pad, free_slot); // LED / luz do controle com o numero do jogador
                    std::printf("[PAD-SDL] controller opened as player %d: %s (type %s, %d touchpad(s))\n", free_slot + 1,
                                name ? name : "?", SDL_GetGamepadStringForType(SDL_GetGamepadType(slot.pad)),
                                SDL_GetNumGamepadTouchpads(slot.pad));
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                for (int i = 0; i < kSlots; ++i) {
                    Slot& slot = g_slots[i];
                    if (slot.pad && slot.id == event.gdevice.which) {
                        std::printf("[PAD-SDL] player %d controller disconnected\n", i + 1);
                        SDL_CloseGamepad(slot.pad);
                        slot.pad = nullptr;
                        std::lock_guard lock(g_mutex);
                        slot.state = State{};
                        slot.name[0] = 0;
                        slot.icons = "";
                        slot.device = Device{};
                    }
                }
            }
        }
        for (Slot& slot : g_slots) {
            if (!slot.pad) {
                continue;
            }
            Poll(slot);
            const u32 rumble = slot.rumble.exchange(0xFFFFFFFF);
            if (rumble != 0xFFFFFFFF) {
                SDL_RumbleGamepad(slot.pad, static_cast<Uint16>(((rumble >> 8) & 0xFF) * 257), static_cast<Uint16>((rumble & 0xFF) * 257), 500);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
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

bool Read(State* out, int slot) {
    if (slot < 0 || slot >= kSlots) {
        *out = State{};
        return false;
    }
    std::lock_guard lock(g_mutex);
    *out = g_slots[slot].state;
    return out->connected;
}

int Count() {
    std::lock_guard lock(g_mutex);
    int count = 0;
    for (const Slot& slot : g_slots) {
        count += slot.state.connected;
    }
    return count;
}

void Rumble(u8 large_motor, u8 small_motor, int slot) {
    if (slot >= 0 && slot < kSlots) {
        g_slots[slot].rumble = (static_cast<u32>(large_motor) << 8) | small_motor;
    }
}

const char* Name(int slot) {
    return slot >= 0 && slot < kSlots ? g_slots[slot].name : "";
}

const char* IconPlatform(int slot) {
    if (slot < 0 || slot >= kSlots) {
        return "";
    }
    std::lock_guard lock(g_mutex);
    return g_slots[slot].icons;
}

bool GetDevice(int slot, Device* out) {
    *out = Device{};
    if (slot < 0 || slot >= kSlots) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (!g_slots[slot].state.connected) {
        return false;
    }
    *out = g_slots[slot].device;
    return true;
}

} // namespace hle::padsdl
