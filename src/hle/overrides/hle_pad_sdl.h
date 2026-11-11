// Controle pelo SDL3 (Fase 24.5): DualShock 4 / DualSense / qualquer gamepad que o SDL reconheca,
// com touchpad. Uma thread propria inicia o SDL, abre os controles que aparecerem (Fase 61: ate 4, um por
// jogador) e guarda o estado mais recente de cada um; scePadReadState so copia esse estado.
#pragma once

#include "common/types.h"

namespace hle::padsdl {

struct State {
    bool connected = false;
    u32 buttons = 0; // ja nas mascaras do Orbis
    u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    u8 l2 = 0, r2 = 0;
    u8 touches = 0;
    struct {
        u16 x, y; // 0..1919, 0..941 (resolucao do touchpad do DualShock 4)
        u8 id;
    } touch[2]{};
};

// Fase 61 (co-op local): ate kSlots controles, cada um numa vaga (0 = jogador 1). Um controle novo ocupa a
// primeira vaga livre; desconectado, a vaga fica livre de novo.
constexpr int kSlots = 4;

void SetEnabled(bool enabled);              // --no-sdl-pad
void Start();                               // idempotente; chamado em scePadInit
bool Read(State* out, int slot = 0);        // false se a vaga nao tem controle aberto pelo SDL
int Count();                                // quantos controles o SDL tem abertos
void Rumble(u8 large_motor, u8 small_motor, int slot = 0);
const char* Name(int slot = 0);             // nome do controle da vaga ("" se nenhum)
// Fase 62: icones que combinam com o controle da vaga: "ps4", "ps3", "360", "xboxone", "wiiu" ("" se nenhum)
const char* IconPlatform(int slot = 0);

// Fase 80: identidade do controle da vaga, para o roster do LegoToypad (hle_pad_share).
struct Device {
    s32 instance_id = -1; // id de instancia do SDL (so vale neste processo)
    u8 guid[16]{};
    u16 vendor = 0, product = 0;
    char path[256] = ""; // SDL_GetGamepadPath: caminho HID ou "XInput#N"
    char name[96] = "";
};
bool GetDevice(int slot, Device* out); // false se a vaga nao tem controle aberto pelo SDL

} // namespace hle::padsdl
