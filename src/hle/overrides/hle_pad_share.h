// Fase 80: entrada por jogador com o app LegoToypad aberto.
//
// O LegoToypad e uma janela por cima do jogo: quem abre o seletor nao pode, ao mesmo tempo, mexer no
// personagem. Antes era tudo ou nada (o app toma o primeiro plano e o PadFocusGate para todos os
// controles). Agora o app publica QUAIS jogadores estao dentro dele, e so a entrada desses fica neutra;
// os outros continuam jogando mesmo com a janela do app na frente.
//
// Dois blocos de memoria compartilhada (CreateFileMapping com nome), structs simples, little-endian:
//   Local\LegoToypadInputOwnership  escrito pelo LegoToypad, lido aqui (quem esta capturado)
//   Local\LegoDimensionsInputRoster escrito aqui, lido pelo LegoToypad (que controle e de que jogador)
// Os dois usam seqlock: `sequence` impar = escrita em andamento; quem le repete ate ler o mesmo valor
// par antes e depois da copia.
//
// Os layouts abaixo sao o contrato com o LegoToypad: podem ser copiados como estao para o outro repo.
#pragma once

#include <cstdint>

namespace hle::padshare {

// ---- Local\LegoToypadInputOwnership (LegoToypad -> jogo) ---------------------------------------
constexpr uint32_t kOwnershipMagic = 0x4950544C; // 'LTPI'
constexpr uint32_t kOwnershipVersion = 1;
constexpr uint32_t kOwnershipFlagPickerVisible = 1u << 0;   // informativo
constexpr uint32_t kOwnershipFlagSneakPeek = 1u << 1;       // informativo; o peek NUNCA captura
constexpr uint32_t kOwnershipFlagForeground = 1u << 2;      // informativo (o jogo confere o primeiro plano sozinho)
constexpr uint32_t kOwnershipFlagShareKeyboard = 1u << 3;   // o teclado vale no jogo com o app na frente (so se o
                                                            // bit do jogador do teclado estiver limpo; ver o roster)

struct ToypadInputOwnership { // 48 bytes
    uint32_t magic;           // kOwnershipMagic
    uint32_t version;         // kOwnershipVersion
    uint32_t sequence;        // seqlock
    uint32_t toypadPid;       // processo do LegoToypad (morto = nada capturado)
    uint64_t heartbeatTickMs; // GetTickCount64() da ultima escrita; mais de 1000 ms atras = nada capturado
    uint32_t capturedPlayers; // bit0 = jogador 1, bit1 = jogador 2, bit2/3 = 3/4 (cada um com o controle da vaga e,
                              // no jogador com kSlotFlagKeyboard no roster, o teclado)
    uint32_t flags;           // kOwnershipFlag*
    uint32_t reserved[4];
};
static_assert(sizeof(ToypadInputOwnership) == 48);

// ---- Local\LegoDimensionsInputRoster (jogo -> LegoToypad) --------------------------------------
constexpr uint32_t kRosterMagic = 0x5247444C; // 'LDGR'
constexpr uint32_t kRosterVersion = 1;
constexpr uint32_t kRosterCapOwnership = 1u << 0;     // o jogo le LegoToypadInputOwnership v1
constexpr uint32_t kRosterCapShareKeyboard = 1u << 1; // o jogo entende kOwnershipFlagShareKeyboard
constexpr uint32_t kRosterCapPadFreeze = 1u << 2;     // Fase 82: frozenZones vale (o jogo sabe quais zonas estao congeladas)

constexpr uint8_t kSlotKindNone = 0;
constexpr uint8_t kSlotKindKeyboard = 1;   // so teclado (o jogador do teclado sem controle)
constexpr uint8_t kSlotKindController = 2; // controle (o jogador do teclado tambem tem o teclado: ver kSlotFlagKeyboard)
constexpr uint8_t kSlotKindVirtual = 3;    // controle de teste (LD_PAD_LIVE2), sem aparelho

constexpr uint8_t kSlotFlagKeyboard = 1u << 0; // o teclado e deste jogador: o 1 ou o 2 (Fase 81: aba Controles do
                                               // launcher ou F3 no jogo); nenhum com --no-keyboard
constexpr uint8_t kSlotFlagSdl = 1u << 1;      // controle aberto pelo SDL3 do jogo (guid / path / vid / pid valem)
constexpr uint8_t kSlotFlagXInput = 1u << 2;   // controle lido pelo XInput (xinputIndex vale)

struct GameInputSlot { // 388 bytes
    uint8_t connected;       // 0/1
    uint8_t kind;            // kSlotKind*
    uint8_t flags;           // kSlotFlag*
    uint8_t reserved0;
    uint8_t joystickGuid[16]; // SDL_GUID do controle (zeros se nao e SDL)
    int32_t sdlInstanceId;    // id de instancia do SDL3 DO JOGO (-1 se nao e SDL); so serve para log:
                              // ids de instancia nao valem entre processos
    int32_t xinputIndex;      // indice do usuario XInput (-1 se nao e XInput)
    uint16_t vendorId;        // USB VID (0 se desconhecido)
    uint16_t productId;       // USB PID (0 se desconhecido)
    uint32_t reserved1;
    char devicePath[256];     // SDL_GetGamepadPath (UTF-8, caminho HID ou "XInput#N"); vazio se desconhecido
    char name[96];            // SDL_GetGamepadName (UTF-8)
};
static_assert(sizeof(GameInputSlot) == 388);

struct GameInputRoster { // 1600 bytes
    uint32_t magic;           // kRosterMagic
    uint32_t version;         // kRosterVersion
    uint32_t sequence;        // seqlock
    uint32_t playerCount;     // quantos slot[i].connected (o jogador 1 e o do teclado sempre contam)
    uint32_t gamePid;         // processo do jogo
    uint32_t capabilities;    // kRosterCap*
    uint64_t heartbeatTickMs; // GetTickCount64() da ultima escrita (a cada 250 ms)
    uint32_t appliedCapture;  // mascara que o jogo esta aplicando agora (depois das checagens de vida)
    uint32_t frozenZones;     // Fase 82 (com kRosterCapPadFreeze; era reserved[0]): zonas do Toy Pad com personagens
                              // presos por um ataque de chefe (AvoidAttack do jogo), bit 0 = zona 1 (centro), bit 1 =
                              // zona 2 (esquerda), bit 2 = zona 3 (direita); 0 = nada. Publicado assim que muda.
    uint32_t reserved[2];
    GameInputSlot slot[4];    // pelo numero de jogador DO JOGO: slot[0] = jogador 1
};
static_assert(sizeof(GameInputRoster) == 1600);

// ---- interface interna ---------------------------------------------------------------------------
void SetEnabled(bool enabled); // --no-toypad-share
void Start();                  // idempotente; chamado em scePadInit

// Jogadores capturados pelo LegoToypad (bit 0 = jogador 1); 0 sem app, com bloco invalido ou velho.
uint32_t CapturedPlayers();
// O bloco do app esta vivo e a janela em primeiro plano e do processo dele: os controles dos jogadores
// nao capturados continuam valendo.
bool PickerInFront();
// O app pediu que o teclado continue no jogo com ele na frente (kOwnershipFlagShareKeyboard); quem chama confere
// se o jogador do teclado nao esta capturado.
bool ShareKeyboardRequested();

} // namespace hle::padshare
