#pragma once

#include "common/types.h"

// Fase 82: ataques de chefe que prendem os personagens de uma zona do Toy Pad ("AvoidAttack" no codigo do jogo).
//
// O chefe escolhe a zona com mais personagens, agarra ate 3 deles e so solta cada um quando a figura dele vai para
// OUTRA zona do Toy Pad (o jogo pisca a zona em vermelho e mostra o aviso AVOIDATTACK_LEFT / MIDDLE / RIGHT). Quase
// todos os chefes usam a mesma classe do jogo (construtor eboot+0x300880, atualizacao eboot+0x301c00); a Bruxa Ma do
// Oeste (fase 1) tem uma copia propria (atualizacao eboot+0x270da0). Os ganchos leem, a cada atualizacao do jogo, a
// zona atacada e as vagas de vitima de cada instancia: o estado vem do proprio jogo, nao da cor dos LEDs.
//
// Enderecos e campos sao do eboot 1.24 (_PATCH11C); com outro eboot, ou outro comeco de funcao, nada e instalado.
namespace hle::avoidattack {

// Poe os ganchos no eboot carregado (antes do Protect). false = eboot diferente: FrozenZones() fica sempre 0.
bool Install(u8* base);
bool Installed();

// Zonas com personagens presos agora: bit 0 = zona 1 (centro), bit 1 = zona 2 (esquerda), bit 2 = zona 3 (direita),
// a mesma numeracao dos LEDs do portal (a zona n do jogo e o bit n).
u32 FrozenZones();

} // namespace hle::avoidattack
