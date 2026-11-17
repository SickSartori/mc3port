/* Started by RecompLauncher, mc3.exe stays as the process it watches and starts each game process in turn
 * (launcher.c). A game process under it (MC3_SUPERVISED) does not start the next one itself: it ends with one of
 * these codes and mc3.exe starts it, so RecompLauncher sees the game end only when the player quits. */
#pragma once

#define MC3_EXIT_RESTART_GAME 0x52530001u     /* the in-game menu's restart: straight into the game */
#define MC3_EXIT_BACK_TO_LAUNCHER 0x52530002u /* "Back to the launcher" */
#define MC3_EXIT_MOVE_OLD_DATA 0x52530003u    /* the launcher's move of the old data folder */
#define MC3_EXIT_RESTART_AFTER_GPU_LOSS 0x52530004u /* xport's device_lost: the same start again, once */
