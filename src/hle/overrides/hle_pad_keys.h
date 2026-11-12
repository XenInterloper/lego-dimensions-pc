// Fase 81: teclado configuravel e o menu de controles (F3).
//
// As teclas, o jogador do teclado e a camera pelo mouse vem da secao [keyboard] do launcher.ini (a aba Controles do
// launcher grava; o menu F3 dentro do jogo tambem). Os nomes das chaves e a ordem das acoes sao o contrato com o
// launcher (tools/dist/launcher.cpp, kKeyActions) e com as dicas de teclado (tools/setup/teclado.py, ACTIONS).
#pragma once

#include <cstddef>

namespace hle::padkeys {

struct Action {
    const char* name;  // chave no launcher.ini
    const char* label; // no menu F3 (maiusculas, sem acento: a fonte do overlay)
    const char* label_pt;
    int default_vk;
};

int ActionCount();
const Action& GetAction(int i);
int Binding(int i);               // codigo de tecla virtual do Windows (mouse: 1, 2, 4, 5, 6); 0 = sem tecla
void SetBinding(int i, int vk);   // a mesma tecla em outra acao sai de la
int TakenBy(int vk, int except);  // a acao que ja usa a tecla (-1 = nenhuma)
void ResetBindings();

int KeyboardPlayer();             // 0 = jogador 1, 1 = jogador 2
void SetKeyboardPlayer(int slot);
bool MouseLook();                 // movimento do mouse = analogico direito (camera)
void SetMouseLook(bool on);
int MouseSensitivity();           // 1..10
void SetMouseSensitivity(int value);

void Save();                      // grava no launcher.ini
void RefreshIcons();              // refaz as dicas de teclado com as teclas atuais (em segundo plano)

bool Reserved(int vk);            // atalhos do port (F1, F2, F3, F8, F9, F10, F11) e as teclas do Windows
void KeyName(int vk, char* out, size_t size); // maiusculas, para o overlay
void OnRawMouse(long dx, long dy);            // WM_INPUT da janela do jogo

} // namespace hle::padkeys
