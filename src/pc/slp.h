/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi-compatible replay recording (src/pc/slp.c). MELEE_SLP_DIR=<dir>
 * writes every VS match -- offline or netplay -- to
 * <dir>/Game_YYYYMMDDTHHMMSS.slp, readable by slippi-js, Slippi Launcher and
 * the tools built on them. Off unless the variable is set.
 *
 * Each tick is captured when it ends and staged by netplay frame; a frame
 * reaches the file only once no rollback can change it (offline, on the next
 * tick), so the file holds exactly one pass per frame. */
#ifndef PC_SLP_H
#define PC_SLP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct HSD_GObj;
struct StartMeleeData;

/* StartMeleeData as Slippi's 0x138-byte Game Info Block and back
 * (slp_format.h's layout). Xelee Watch's match header carries it
 * (src/pc/net_watch.c). `resolve_alt` records the character that starts
 * when A is held on Zelda or Sheik, as the .slp writer does; without it the
 * block holds the character picked, which the match setup then resolves
 * itself. The callback pointers are not carried, and decoding leaves them as
 * they were. */
void pc_slp_info_from_start(
    uint8_t out[0x138], const struct StartMeleeData* data, bool resolve_alt);
void pc_slp_start_from_info(struct StartMeleeData* start, const uint8_t in[0x138]);

/* A VS or Sudden Death scene is being set up (gmvs.c fn_8016E730, where
 * Slippi's SendGameInfo hooks): opens a file if this match is recorded. */
void pc_slp_match_start(const struct StartMeleeData* data);
/* The match scene is leaving (gmvs.c gm_Scene_Vs_OnExit): every staged frame
 * is written, Game End included if it happened, and the file is closed. */
void pc_slp_match_end(void);
/* Around every simulation tick, fresh or re-run (gmscene.c gm_RunSimTick).
 * `proc_mask` is the GObj p_link mask the tick ran its procs under; a tick
 * whose mask held the fighters (pause, GAME! freeze) is no replay frame. */
void pc_slp_tick_begin(void);
void pc_slp_tick_end(uint64_t proc_mask);
/* A fighter's inputs for the tick are read (fighter.c
 * Fighter_Spaghetti_8006AD10, where Slippi's SendGamePreFrame hooks): the
 * pre-frame event's state and inputs are taken here. */
void pc_slp_pre_frame(struct HSD_GObj* gobj);
/* An aerial is landing (ftCo_LandingAir.c, where Slippi stamps the
 * post-frame's L-cancel status): whether it was in the window. */
void pc_slp_l_cancel(struct HSD_GObj* gobj, bool in_window);
/* Slippi's SyncRNG has just seeded this tick (gmvs.c slippiSyncRng): in an
 * online match the frame-start event carries this seed, which Slippi reads
 * after SyncRNG has run, not the one the tick began with. */
void pc_slp_sync_rng(uint32_t seed);

/* ---- playback ---------------------------------------------------------
 * MELEE_SLP_PLAY=<file.slp> (src/pc/slp_play.c): plays a Slippi replay's
 * match back through the game, as Slippi online played it -- the match set
 * up from the replay's game start (with MELEE_BOOT_SCENE=vs), SyncRNG from
 * its rngOffset, UCF and the online rules on (a deterministic run), and
 * every fight frame's physical pads fed in before its tick: the raw main
 * stick, the button word, the raw C-stick and triggers (or, before Slippi
 * 3.17, the raw values the recorded normalized ones come from). With
 * MELEE_SLP_DIR set the run records its own replay, to set beside the
 * original (tools/slp_compare.py): the first frame they part is where this
 * game differs from Dolphin's. The game leaves once the replay is done. */
struct PADStatus;
bool pc_slp_play_active(void);
/* The replay's rngOffset (the seed its game start carries). */
uint32_t pc_slp_play_rng_offset(void);
/* Whether the replay's Pokemon Stadium is frozen (false: not playing one). */
bool pc_slp_play_frozen_stadium(bool* frozen);
/* gmvsmode.c fillDebugVs: the debug match becomes the replay's. */
void pc_slp_play_setup(struct StartMeleeData* start);
/* The VS scene has set up (pc_slp_match_start): fight frames count from
 * here. */
void pc_slp_play_match_start(void);
/* net.c fresh tick: the next tick's pads into the queue head. */
void pc_slp_play_feed(struct PADStatus* head);
/* The match scene is leaving (pc_slp_match_end): the game leaves too,
 * the replay it recorded written. */
void pc_slp_play_match_end(void);
/* After every tick (pc_slp_tick_end): whether the fighters ran in it. */
void pc_slp_play_tick_end(bool fight_frame);

/* net_snapshot.c, for the recorder. The netplay frame number of the tick
 * running: a rollback's re-run frame, otherwise the newest fresh one (it
 * counts every tick offline too). */
int32_t pc_net_sim_frame(void);
/* The newest netplay frame no rollback can change (record_confirm);
 * INT32_MIN when no session is running. */
int32_t pc_net_confirmed_frame(void);

#ifdef __cplusplus
}
#endif

#endif
