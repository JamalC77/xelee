/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Xelee Watch: a headset nearby plays the host's VS match live, from the
 * host's per-tick inputs (src/pc/net_watch.c; spectator-spec.md).
 *
 * The host sends a match header when a VS match is set up and one frame
 * record per simulated tick; the viewer runs the same match from the
 * header, writing the host's pads and seed into each of its own ticks, and
 * draws it from its own placement and head. The game thread never touches
 * the network: it appends to (host) or reads from (viewer) logs a thread of
 * this module serves, so the host never waits on a viewer.
 *
 * Game-facing and C only; every function runs on the game thread unless it
 * says otherwise. */
#ifndef PC_NET_WATCH_H
#define PC_NET_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PADStatus;
struct StartMeleeData;

#define PC_WATCH_INFO_SIZE 0x138 /* StartMeleeData as Slippi's Game Info Block */
#define PC_WATCH_NAME_LEN 24
#define PC_WATCH_MSG_LEN 96

/* ---- both roles ---------------------------------------------------------- */

/* Once per present (src/pc/vi.c): starts sharing once a disc is open. */
void pc_watch_service(void);
/* The launcher's "Share to watchers"; MELEE_WATCH_SHARE=0|1 overrides it. */
void pc_watch_set_share(bool on);
/* A VS or Sudden Death match is being set up (gmvs.c fn_8016E730, before
 * any of it draws from the RNG). The host captures the header; the viewer
 * writes the taken header's match over `start` and the game's pad state. */
void pc_watch_match_start(struct StartMeleeData* start);
/* The match scene is leaving (gmvs.c gm_Scene_Vs_OnExit). */
void pc_watch_match_end(void);
/* A shared (host) or watched (viewer) match is running: the run must then
 * reproduce on the other device (pc_net_deterministic). Any thread. */
bool pc_watch_live(void);
/* Viewer, inside a watched match. Any thread. */
bool pc_watch_viewing(void);

/* ---- host, per tick (src/pc/net.c fresh_tick) --------------------------- */

/* The tick's four pads as simulated, its checksum and its frame number. */
void pc_watch_tx_frame(const struct PADStatus* head, uint32_t ck, int32_t net_frame);

/* ---- viewer, per scene-loop pass and per tick --------------------------- */

/* Once per scene-loop pass (gmscene.c): true while the next host frame is
 * not in yet (or the start buffer is not full), so the pass draws without
 * ticking; the local pads that came in are dropped. */
bool pc_watch_hold(void);
/* While held: the host's match is over and every frame of it has run, so
 * the scene ends now (also when the player held B to leave). */
bool pc_watch_end_scene_now(void);
/* Before the checksum of a fresh tick (net.c): the viewer writes the next
 * host frame's pads and seed into the queue head; the host clears the pads'
 * padding byte, which the checksum covers and no field does. */
void pc_watch_feed(struct PADStatus* head);
/* The viewer's checksum of the tick just fed, against the host's. */
void pc_watch_compare(uint32_t ck);
/* The scene has asked to end (pc_net_scene_hold): true while it must keep
 * ticking because the host's match ran on past this frame. */
bool pc_watch_scene_hold(int32_t net_frame);
/* The host has been silent for 5 s, or said goodbye. */
bool pc_watch_lost(void);

/* ---- viewer client, lobby ------------------------------------------------- */

/* A match whose header is in, for the lobby to set up (gmonlinemode.c). */
typedef struct PcWatchMatch {
    uint32_t seq;
    uint8_t scene_kind; /* GS_VS or GS_SUDDEN_DEATH */
    uint16_t stkind;
    uint8_t info[PC_WATCH_INFO_SIZE]; /* pc_slp_start_from_info() */
} PcWatchMatch;

/* What the waiting screen shows. */
typedef struct PcWatchStatus {
    char host[PC_WATCH_NAME_LEN]; /* the host's name, its address until known */
    char message[PC_WATCH_MSG_LEN];
    bool error; /* the message is a problem */
} PcWatchStatus;

/* The Watch lobby opens: the viewer role starts (MELEE_WATCH_HOST
 * subscribes, MELEE_WATCH_FILE plays a dump). Idempotent: the subscription
 * outlives the lobby while matches come and go. */
void pc_watch_client_start(void);
/* Leaving Watch: goodbye to the host, the viewer role ends. */
void pc_watch_client_stop(void);
/* Watch the host at ip (IPv4 text) and its stream port. */
bool pc_watch_subscribe(const char* ip, uint16_t port);
void pc_watch_status(PcWatchStatus* out);
/* The oldest match whose header is in and that has not been played: true
 * with it in *m. It is the one the next pc_watch_match_start() plays. */
bool pc_watch_take_match(PcWatchMatch* m);
/* Before leaving the lobby for a taken match: the host's GameRules, item
 * and stage switches, unlock state and frozen Stadium replace ours, kept
 * aside, until pc_watch_unpin_rules() (lobby entry, leaving Watch). */
void pc_watch_pin_rules(const PcWatchMatch* m);
void pc_watch_unpin_rules(void);
/* The launcher's unlock-all and frozen-Stadium answers while pinned
 * (src/pc/launcher.cpp); false when nothing is pinned. Any thread. */
bool pc_watch_rules(bool* unlock_all, bool* frozen_stadium);

#ifdef __cplusplus
}
#endif

#endif
