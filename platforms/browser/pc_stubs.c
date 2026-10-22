/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Native-only pieces of src/pc that the browser platform replaces.
 *
 * The desktop launcher (launcher.cpp), the libusb GameCube adapter
 * (gcadapter.c) and the archive file cache (file_cache.cpp) have no browser
 * counterpart: settings come from the hosting page through the environment,
 * pads arrive through SDL, and disc reads are cached by
 * disc-cache.mjs. Everything else in src/pc is compiled unchanged.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/file_cache.h"
#include "pc/net_watch.h"
#include "pc/pc.h"
#include "pc/slp.h"

/* Desktop launcher preferences; defaults match launcher_data.hpp. MELEE_UCF is
 * the one the launcher also reads from the environment (launcher.cpp). */
bool pc_is_unlock_all_enabled(void) {
    return false;
}
bool pc_is_frozen_stadium_enabled(void) {
    return false;
}
bool pc_is_free_camera_enabled(void) {
    return false;
}
bool pc_is_ucf_enabled(void) {
    const char* env = getenv("MELEE_UCF");
    return env != NULL && env[0] != '0';
}
int pc_get_hud_mode(void) {
    return 0;
}
bool pc_is_custom_textures_enabled(void) {
    return false;
}
/* Netplay lobby identity and Direct Connect settings (net_lan.c, net_match.c); the lobby itself is
 * unreachable here. */
const char* pc_get_net_name(void) {
    return "";
}
const char* pc_get_net_target(void) {
    return "";
}
void pc_set_net_target(const char* code) {
    (void)code;
}
int pc_get_net_port(void) {
    return 0;
}
uint64_t pc_install_id(void) {
    return 0;
}
const char* pc_app_rev(void) {
    return "browser";
}

/* libusb GameCube adapter. */
void pc_gcadapter_init(void) {}
void pc_gcadapter_poll(void) {}
void pc_gcadapter_apply(void) {}
bool pc_gcadapter_status(int port, struct PADStatus* out) {
    (void)port;
    (void)out;
    return false;
}
bool pc_gcadapter_raw(int port, uint8_t raw[6], bool* wireless) {
    (void)port;
    (void)raw;
    (void)wireless;
    return false;
}
uint64_t pc_gcadapter_report_count(void) {
    return 0;
}
void pc_gcadapter_motor(int slot, unsigned command) {
    (void)slot;
    (void)command;
}

/* Archive cache and background prewarm: the page owns disc caching. */
bool pc_file_cache_get(const char* filename, void* dst, size_t* size) {
    (void)filename;
    (void)dst;
    (void)size;
    return false;
}
bool pc_file_cache_get_size(const char* filename, size_t* size) {
    (void)filename;
    (void)size;
    return false;
}
void pc_file_cache_put(const char* filename, const void* data, size_t size) {
    (void)filename;
    (void)data;
    (void)size;
}
void pc_file_cache_start_prewarm(void) {}
bool pc_file_cache_require(const char* filename) {
    (void)filename;
    return false;
}

/* Replay recording writes to a native file; the browser has no recorder. */
void pc_slp_match_start(const struct StartMeleeData* data) {
    (void)data;
}
void pc_slp_match_end(void) {}
void pc_slp_tick_begin(void) {}
void pc_slp_tick_end(uint64_t proc_mask) {
    (void)proc_mask;
}
void pc_slp_pre_frame(struct HSD_GObj* gobj) {
    (void)gobj;
}
void pc_slp_sync_rng(uint32_t seed) {
    (void)seed;
}
/* Nor replay playback, which reads one. */
bool pc_slp_play_active(void) {
    return false;
}
uint32_t pc_slp_play_rng_offset(void) {
    return 0;
}
bool pc_slp_play_frozen_stadium(bool* frozen) {
    (void)frozen;
    return false;
}
void pc_slp_play_setup(struct StartMeleeData* start) {
    (void)start;
}
void pc_slp_play_match_start(void) {}
void pc_slp_play_feed(struct PADStatus* head) {
    (void)head;
}
void pc_slp_play_match_end(void) {}
void pc_slp_play_tick_end(bool fight_frame) {
    (void)fight_frame;
}
void pc_slp_info_from_start(
    uint8_t out[0x138], const struct StartMeleeData* data, bool resolve_alt) {
    (void)data;
    (void)resolve_alt;
    memset(out, 0, 0x138);
}
void pc_slp_start_from_info(struct StartMeleeData* start, const uint8_t in[0x138]) {
    (void)start;
    (void)in;
}

/* Xelee Watch (net_watch.c) runs a UDP socket on a thread of its own; the
 * browser neither shares its matches nor watches another's. */
void pc_watch_service(void) {}
void pc_watch_set_share(bool on) {
    (void)on;
}
void pc_watch_shutdown(void) {}
void pc_watch_match_start(struct StartMeleeData* start) {
    (void)start;
}
void pc_watch_match_end(void) {}
bool pc_watch_live(void) {
    return false;
}
bool pc_watch_viewing(void) {
    return false;
}
void pc_watch_tx_frame(const struct PADStatus* head, uint32_t ck, int32_t net_frame) {
    (void)head;
    (void)ck;
    (void)net_frame;
}
bool pc_watch_hold(void) {
    return false;
}
bool pc_watch_end_scene_now(void) {
    return false;
}
void pc_watch_feed(struct PADStatus* head) {
    (void)head;
}
void pc_watch_compare(uint32_t ck) {
    (void)ck;
}
bool pc_watch_scene_hold(int32_t net_frame) {
    (void)net_frame;
    return false;
}
bool pc_watch_lost(void) {
    return false;
}
void pc_watch_client_start(void) {}
void pc_watch_client_stop(void) {}
bool pc_watch_subscribe(const char* ip, uint16_t port) {
    (void)ip;
    (void)port;
    return false;
}
void pc_watch_status(PcWatchStatus* out) {
    memset(out, 0, sizeof *out);
    snprintf(out->message, sizeof out->message, "Watch is not available in the browser");
    out->error = true;
}
bool pc_watch_take_match(PcWatchMatch* m) {
    (void)m;
    return false;
}
void pc_watch_pin_rules(const PcWatchMatch* m) {
    (void)m;
}
void pc_watch_unpin_rules(void) {}
bool pc_watch_rules(bool* unlock_all, bool* frozen_stadium) {
    (void)unlock_all;
    (void)frozen_stadium;
    return false;
}
