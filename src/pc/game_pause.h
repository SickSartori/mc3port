#pragma once

// The game stopped and resumed by the port (the in-game menu): its threads wait at their next kernel call, its
// timers, DPCs and GPU interrupts stop, its clocks leave the paused time out and the sound goes quiet. The GPU
// thread keeps presenting the last frame, with the menu over it.

#include <cstdint>

namespace mc3::pc {

void game_pause(bool paused);
bool game_paused();

// Waits here while the game is paused. Only where the calling thread holds no host lock (kernel calls' entry and
// exit, the DPC and timer threads between items).
void game_pause_point();

// Performance-counter ticks the game has spent paused, the current pause included: the game's clocks subtract them.
int64_t game_paused_ticks();

}  // namespace mc3::pc
