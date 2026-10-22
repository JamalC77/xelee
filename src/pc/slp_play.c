/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi replay playback; see slp.h. */
#include "compat.h"
#include "pc/pc.h"
#include "pc/slp.h"
#include "pc/slp_format.h"
#include <dolphin/pad.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>
#include <melee/ft/fighter.h>
#include <melee/ft/types.h>
#include <melee/mn/types.h>

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern bool pc_exit_requested;

/* Frames past the replay's last that the game runs before it leaves, so
 * the replay it records ends as the original did. */
#define TAIL_FRAMES 30

typedef struct PlayFrame {
    PADStatus pad[4];
    float c[4][2];  /* the C-stick as the game normalized it */
    uint32_t cdir[4]; /* and the direction bits it set (HSD_PadADConvert) */
    float lr[4][2]; /* the triggers likewise (nml_analogL, nml_analogR) */
    float pre_pos[4][2];  /* the fighter's position going in */
    float post_pos[4][2]; /* and coming out */
    float hitlag[4];      /* its hitlag coming out */
    float fc[4][2];       /* the follower's (Nana's) C-stick as the game read it */
    bool have[4], have_post[4], have_f[4];
} PlayFrame;

static struct {
    int state; /* 0 not looked at, 1 playing, -1 off */
    uint8_t info[SLP_INFO_BLOCK_SIZE];
    uint32_t seed; /* the game start's: Slippi's rngOffset (SyncRNG) */
    bool frozen_ps; /* the game start's frozen Pokemon Stadium */
    bool raw_c;    /* the replay has the raw C-stick (3.17 on) */
    int32_t last;  /* the replay's last frame */
    int32_t count; /* frames held, from SLP_FIRST_FRAME */
    PlayFrame* frames;
    bool present[4]; /* the port has a fighter somewhere in the replay */
    bool started;  /* the VS scene has set up */
    int32_t next;  /* the fight frame the next tick is */
    bool converted;
    unsigned c_misses, lr_misses, c_snaps_read, c_snaps_nana;
} P;

/* Event fields, as offsets from the command byte (SPEC.md). */
#define START_SEED 0x13D
#define START_FROZEN_PS 0x1A2
#define PRE_FRAME 0x1
#define PRE_PORT 0x5
#define PRE_FOLLOWER 0x6
#define PRE_X 0xD
#define PRE_Y 0x11
#define PRE_C_X 0x21
#define PRE_C_Y 0x25
#define PRE_BUTTONS 0x2D
#define C_DIRS 0x00F00000u /* the C-stick's direction bits in the processed buttons */
#define PRE_PHYS_BUTTONS 0x31
#define PRE_PHYS_L 0x33
#define PRE_PHYS_R 0x37
#define PRE_RAW_X 0x3B
#define PRE_RAW_Y 0x40
#define PRE_RAW_CX 0x41
#define PRE_RAW_CY 0x42
#define POST_X 0xA
#define POST_Y 0xE
#define POST_HITLAG 0x49

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static float bef(const uint8_t* p) {
    const uint32_t u = be32(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* The C-stick and triggers come back as the game normalized them, so the
 * raw values that normalize so are found by running the pad library's own
 * clamp, direction bits and scale over every raw value
 * (HSD_PadPcNormalize), once it has its settings. Several raws normalize
 * alike (inside the deadzone, past the rim): of those that also set the
 * direction bits the replay's buttons show, one stands for them all. Any
 * gives the game the same normalized stick, but not the same raw one, and
 * UCF reads the raw: past the rim with the other axis near the centre it
 * snaps the stick to exactly 1.0 (ftucf.c), which automatic smash DI then
 * moves the fighter by. A stick held off centre is almost always at the
 * rim, and one held along an axis in its notch: for those a raw UCF
 * snaps stands for them if there is one, then the largest; at rest, (0, 0),
 * the smallest. Where the replay shows which it was, that one
 * (c_snaps_from_asdi). (Replays from Slippi 3.17 on carry the raw C-stick
 * itself.) A table keyed by the two floats' bits and the direction bits,
 * holding the best raw that snaps and the best that does not. */
#define C_SLOTS (1u << 17)
static struct {
    uint64_t key; /* x bits << 32 | y bits, plus one: 0 is empty */
    uint32_t dir;
    int8_t x, y;   /* the best that does not snap */
    int8_t sx, sy; /* the best that snaps */
    bool plain, snap;
} s_c[C_SLOTS];
static float s_lr[256]; /* nml_analogL for each raw trigger value */

static uint64_t c_key(float x, float y) {
    uint32_t a, b;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    return (((uint64_t)a << 32) | b) + 1;
}

static uint32_t c_hash(uint64_t k) {
    return (uint32_t)(k ^ (k >> 29) ^ (k >> 47)) * 2654435761u;
}

static void build_tables(void) {
    memset(s_c, 0, sizeof s_c);
    for (int x = -128; x < 128; x++) {
        for (int y = -128; y < 128; y++) {
            HSD_PadStatus mp;
            memset(&mp, 0, sizeof mp);
            mp.subStickX = (s8)x;
            mp.subStickY = (s8)y;
            HSD_PadPcNormalize(&mp);
            /* The replay holds the fighter's C-stick, which Fighter_procInput
             * has put through its deadzones after the pad library set the
             * direction bits: a raw (-28, 20) reads (-0.35, 0) with the
             * stick's left bit set. */
            float nx = mp.nml_subStickX, ny = mp.nml_subStickY;
            if (fabsf(nx) <= p_ftCommonData->horizontal_stick_deadzone) {
                nx = 0.0f;
            }
            if (fabsf(ny) <= p_ftCommonData->vertical_stick_deadzone) {
                ny = 0.0f;
            }
            const uint64_t k = c_key(nx, ny);
            const uint32_t dir = mp.button & C_DIRS;
            for (uint32_t h = c_hash(k ^ dir);; h++) {
                const uint32_t i = h & (C_SLOTS - 1);
                if (s_c[i].key == 0 || (s_c[i].key == k && s_c[i].dir == dir)) {
                    /* UCF's cardinal snap (ftucf.c snapCardinal): at the rim on
                     * one axis, within 6 of centre on the other */
                    const bool snaps = (abs(x) >= 80 && abs(y) <= 6) || (abs(y) >= 80 && abs(x) <= 6);
                    const bool rest = nx == 0.0f && ny == 0.0f;
                    const int m = abs(x) + abs(y);
                    if (snaps) {
                        if (!s_c[i].snap || m > abs(s_c[i].sx) + abs(s_c[i].sy)) {
                            s_c[i].sx = (int8_t)x;
                            s_c[i].sy = (int8_t)y;
                            s_c[i].snap = true;
                        }
                    } else {
                        const int held = abs(s_c[i].x) + abs(s_c[i].y);
                        if (!s_c[i].plain || (rest ? m < held : m > held)) {
                            s_c[i].x = (int8_t)x;
                            s_c[i].y = (int8_t)y;
                            s_c[i].plain = true;
                        }
                    }
                    s_c[i].key = k;
                    s_c[i].dir = dir;
                    break;
                }
            }
        }
    }
    for (int v = 0; v < 256; v++) {
        HSD_PadStatus mp;
        memset(&mp, 0, sizeof mp);
        mp.analogL = (u8)v;
        HSD_PadPcNormalize(&mp);
        s_lr[v] = mp.nml_analogL;
    }
}

/* snap: 1 a raw UCF snaps, 0 one it does not, -1 the default (a snapping
 * one if there is one). False if there is none such. */
static bool c_raw(float x, float y, uint32_t dir, int snap, s8* rx, s8* ry) {
    const uint64_t k = c_key(x, y);
    for (uint32_t h = c_hash(k ^ dir);; h++) {
        const uint32_t i = h & (C_SLOTS - 1);
        if (s_c[i].key == 0) {
            return false;
        }
        if (s_c[i].key == k && s_c[i].dir == dir) {
            const bool use_snap = snap < 0 ? s_c[i].snap : snap > 0;
            if (use_snap ? !s_c[i].snap : !s_c[i].plain) {
                return false;
            }
            *rx = use_snap ? s_c[i].sx : s_c[i].x;
            *ry = use_snap ? s_c[i].sy : s_c[i].y;
            return true;
        }
    }
}

/* Whether UCF snapped a held C-stick shows in the replay where the stick
 * moved the fighter: at hitlag's end ASDI (ftCo_Damage_OnExitHitlag) moves
 * it by the C-stick times x4BC, between the frame's post-frame position
 * before and its pre-frame one; snapped, by exactly x4BC along the axis.
 * The stick is the one that frame before held (a5 001d1f12: -0.9875 held
 * through hitlag, -0.975 the frame after). The raw that agrees goes to the
 * whole hold: a held stick does not move. */
/* Popo's C-stick as Nana gets it (ftCo_800B0918, ftCo_GetCpuCStickY): the
 * fighter's stick after UCF, as an s8, then back to a float. */
static float nana_axis(float c) {
    const int8_t q = c >= 0.0f ? (int8_t)(uint8_t)(127.0f * c) : (int8_t)(128.0f * c);
    return q > 0 ? q / 127.0f : q / 128.0f;
}

/* Ice Climbers: Nana copies Popo's C-stick a few frames later from what
 * Popo's fighter read after UCF, snapped or not, so her recorded stick
 * says which. The lag is found from the frames that hold no doubt. A hold
 * at the rim can snap on some frames and not others (the other axis
 * wobbles inside the deadzone), so a frame Nana answers for takes the raw
 * she shows; the rest of the hold, the one most of her frames show. */
static void c_snaps_from_nana(void) {
    for (int port = 0; port < 4; port++) {
        int best_lag = -1, best = 0;
        for (int lag = 1; lag <= 12; lag++) {
            int agree = 0;
            for (int32_t i = 0; i + lag < P.count; i++) {
                const PlayFrame* a = &P.frames[i];
                const PlayFrame* b = &P.frames[i + lag];
                if (!a->have[port] || !b->have_f[port] || (a->c[port][0] == 0.0f && a->c[port][1] == 0.0f)) {
                    continue;
                }
                agree += nana_axis(a->c[port][1]) == b->fc[port][1] && nana_axis(a->c[port][0]) == b->fc[port][0];
            }
            if (agree > best) {
                best = agree;
                best_lag = lag;
            }
        }
        if (best_lag < 0 || best < 20) {
            continue;
        }
        for (int32_t a = 0; a < P.count;) {
            const PlayFrame* fr = &P.frames[a];
            const float cx = fr->c[port][0], cy = fr->c[port][1];
            int32_t b = a;
            while (b + 1 < P.count && P.frames[b + 1].have[port] && P.frames[b + 1].c[port][0] == cx &&
                   P.frames[b + 1].c[port][1] == cy && P.frames[b + 1].cdir[port] == fr->cdir[port]) {
                b++;
            }
            const int axis = fabsf(cx) >= fabsf(cy) ? 0 : 1;
            const float c = fr->c[port][axis];
            if (fr->have[port] && c != 0.0f && fabsf(c) != 1.0f) {
                int snap = 0, plain = 0;
                for (int32_t j = a; j <= b && j + best_lag < P.count; j++) {
                    const PlayFrame* n = &P.frames[j + best_lag];
                    if (!n->have_f[port]) {
                        continue;
                    }
                    const float v = n->fc[port][axis];
                    snap += v == (c < 0 ? -1.0f : 1.0f) && n->fc[port][1 - axis] == 0.0f;
                    plain += v == nana_axis(c);
                }
                s8 rx, ry, sx, sy, px, py;
                const bool have_s = c_raw(cx, cy, fr->cdir[port], 1, &sx, &sy);
                const bool have_p = c_raw(cx, cy, fr->cdir[port], 0, &px, &py);
                if (snap != plain && c_raw(cx, cy, fr->cdir[port], snap > plain, &rx, &ry)) {
                    for (int32_t j = a; j <= b; j++) {
                        P.frames[j].pad[port].substickX = rx;
                        P.frames[j].pad[port].substickY = ry;
                    }
                    P.c_snaps_nana++;
                }
                for (int32_t j = a; j <= b && j + best_lag < P.count && have_s && have_p; j++) {
                    const PlayFrame* n = &P.frames[j + best_lag];
                    if (!n->have_f[port]) {
                        continue;
                    }
                    const float v = n->fc[port][axis];
                    if (v == (c < 0 ? -1.0f : 1.0f) && n->fc[port][1 - axis] == 0.0f) {
                        P.frames[j].pad[port].substickX = sx;
                        P.frames[j].pad[port].substickY = sy;
                    } else if (v == nana_axis(c)) {
                        P.frames[j].pad[port].substickX = px;
                        P.frames[j].pad[port].substickY = py;
                    }
                }
            }
            a = b + 1;
        }
        pc_log_line("slp play: port %d's follower copies its C-stick %d frames later (%d frames agree)", port,
            best_lag, best);
    }
}

static void c_snaps_from_asdi(void) {
    const float k = p_ftCommonData->x4BC;
    const float min = p_ftCommonData->sdi_min_stick_mag;
    /* the frames whose raw an ASDI showed, and that raw */
    static int32_t ev_frame[4096];
    static s8 ev_x[4096], ev_y[4096];
    for (int port = 0; port < 4; port++) {
        int n = 0;
        for (int32_t i = 1; i < P.count && n < (int)(sizeof ev_frame / sizeof *ev_frame); i++) {
            const PlayFrame* fr = &P.frames[i];
            const PlayFrame* pv = &P.frames[i - 1];
            if (!fr->have[port] || !pv->have[port] || !pv->have_post[port] ||
                !(pv->hitlag[port] > 0.0f && pv->hitlag[port] <= 1.0f)) {
                continue;
            }
            const float cx = pv->c[port][0], cy = pv->c[port][1];
            if (cx * cx + cy * cy < min * min) {
                continue;
            }
            const int axis = fabsf(cx) >= fabsf(cy) ? 0 : 1;
            const float c = pv->c[port][axis];
            if (fabsf(c) == 1.0f) {
                continue;
            }
            const float d = fr->pre_pos[port][axis] - pv->post_pos[port][axis];
            const float off_snap = fabsf(d - copysignf(k, c)), off_plain = fabsf(d - c * k);
            if (fminf(off_snap, off_plain) > 0.01f) {
                continue; /* something else moved it too */
            }
            const int snap = off_snap < off_plain;
            if (!c_raw(cx, cy, pv->cdir[port], snap, &ev_x[n], &ev_y[n])) {
                continue;
            }
            ev_frame[n++] = i - 1;
            P.c_snaps_read++;
        }
        /* Each frame of a hold takes the raw of the nearest frame an ASDI
         * read in it: the stick can wobble across UCF's snap inside a hold
         * the replay shows as one value (a5 00298f66: plain at one hitlag's
         * end, snapped at the next two). */
        for (int e = 0; e < n;) {
            const int32_t at = ev_frame[e];
            const PlayFrame* fr = &P.frames[at];
            const float cx = fr->c[port][0], cy = fr->c[port][1];
            int32_t a = at, b = at;
            while (a > 0 && P.frames[a - 1].have[port] && P.frames[a - 1].c[port][0] == cx &&
                   P.frames[a - 1].c[port][1] == cy && P.frames[a - 1].cdir[port] == fr->cdir[port]) {
                a--;
            }
            while (b + 1 < P.count && P.frames[b + 1].have[port] && P.frames[b + 1].c[port][0] == cx &&
                   P.frames[b + 1].c[port][1] == cy && P.frames[b + 1].cdir[port] == fr->cdir[port]) {
                b++;
            }
            int last = e;
            while (last + 1 < n && ev_frame[last + 1] <= b) {
                last++;
            }
            for (int32_t j = a, cur = e; j <= b; j++) {
                while (cur < last && abs(ev_frame[cur + 1] - j) < abs(ev_frame[cur] - j)) {
                    cur++;
                }
                P.frames[j].pad[port].substickX = ev_x[cur];
                P.frames[j].pad[port].substickY = ev_y[cur];
            }
            e = last + 1;
        }
    }
}

static bool lr_raw(float v, u8* raw) {
    for (int i = 0; i < 256; i++) {
        if (s_lr[i] == v) {
            *raw = (u8)i;
            return true;
        }
    }
    return false;
}

/* Slippi 3.16's online codes froze Pokemon Stadium outright (a branch
 * written over grStadium_801D4548's transformation at 0x801D4610), before
 * the game start carried a frozen flag: a replay that ran them had it
 * frozen. Walks the Gecko list for that write. */
static bool gecko_freezes_stadium(const uint8_t* g, size_t n) {
    size_t i = 0;
    while (i + 8 <= n) {
        const uint32_t w0 = be32(g + i), w1 = be32(g + i + 4);
        const uint8_t t = (uint8_t)(w0 >> 24);
        if (t == 0xC2 || t == 0xC3) {
            i += 8 + (size_t)w1 * 8;
        } else if (t == 0x04 || t == 0x05) {
            if ((0x80000000u | (w0 & 0x01FFFFFFu)) == 0x801D4610u && w1 == 0x4800004Cu) {
                return true;
            }
            i += 8;
        } else if (t == 0x06 || t == 0x07) {
            i += 8 + ((size_t)w1 + 7) / 8 * 8;
        } else if (w0 == 0xF0000000u) {
            break;
        } else {
            i += 8;
        }
    }
    return false;
}

static bool load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        pc_log_line("slp play: cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = size > 15 ? malloc((size_t)size) : NULL;
    const bool read = data != NULL && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read || memcmp(data, "{U\x03raw[$U#l", 11) != 0) {
        pc_log_line("slp play: %s is not a Slippi replay", path);
        free(data);
        return false;
    }
    const uint32_t raw_len = be32(data + 11);
    const uint8_t* e = data + 15;
    const uint8_t* end = e + (raw_len <= (uint32_t)(size - 15) ? raw_len : (uint32_t)(size - 15));
    uint16_t sizes[256] = {0};
    bool have_start = false;
    uint8_t* split = NULL; /* the Message Splitter's chunks of the Gecko list */
    size_t split_len = 0;
    int32_t cap = 0;
    while (e < end) {
        const uint8_t cmd = e[0];
        if (cmd == SLP_CMD_EVENT_PAYLOADS) {
            const uint8_t n = e[1];
            for (int k = 2; k + 2 <= n && e + k + 2 < end; k += 3) {
                sizes[e[k]] = (uint16_t)((e[k + 1] << 8) | e[k + 2]);
            }
            e += 1 + n;
            continue;
        }
        if (sizes[cmd] == 0 || e + 1 + sizes[cmd] > end) {
            break; /* an event without a size, or cut short: the replay ends here */
        }
        const size_t len = 1u + sizes[cmd];
        if (cmd == 0x10 && len >= 0x205) {
            /* Message Splitter: 512 data bytes, their used size, the inner
             * command, whether it is the last chunk */
            const uint16_t used = (uint16_t)((e[0x201] << 8) | e[0x202]);
            uint8_t* grown = realloc(split, split_len + used);
            if (grown != NULL && used <= 512) {
                split = grown;
                memcpy(split + split_len, e + 1, used);
                split_len += used;
            }
            if (e[0x204] && e[0x203] == 0x3D) {
                if (gecko_freezes_stadium(split, split_len)) {
                    P.frozen_ps = true;
                    pc_log_line("slp play: the replay's codes freeze Pokemon Stadium (3.16's online codes)");
                }
                split_len = 0;
            }
        } else if (cmd == SLP_CMD_GAME_START && len >= START_SEED + 4) {
            memcpy(P.info, e + 5, SLP_INFO_BLOCK_SIZE);
            P.seed = be32(e + START_SEED);
            P.frozen_ps = P.frozen_ps || (len > START_FROZEN_PS && e[START_FROZEN_PS] != 0);
            have_start = true;
            pc_log_line("slp play: %s, Slippi %u.%u.%u, stage %u, rngOffset %08x", path, e[1], e[2], e[3],
                (unsigned)((e[5 + 0xE] << 8) | e[5 + 0xF]), P.seed);
        } else if (cmd == SLP_CMD_PRE_FRAME && len > PRE_RAW_X && !e[PRE_FOLLOWER] && e[PRE_PORT] < 4) {
            const int32_t idx = (int32_t)be32(e + PRE_FRAME) - SLP_FIRST_FRAME;
            if (idx >= 0 && idx < 1000000) {
                if (idx >= cap) {
                    const int32_t ncap = idx + 4096;
                    PlayFrame* grown = realloc(P.frames, sizeof *grown * (size_t)ncap);
                    if (grown == NULL) {
                        break;
                    }
                    memset(grown + cap, 0, sizeof *grown * (size_t)(ncap - cap));
                    P.frames = grown;
                    cap = ncap;
                }
                PlayFrame* fr = &P.frames[idx];
                const int port = e[PRE_PORT];
                PADStatus* pad = &fr->pad[port];
                /* The physical pad: its button word, the raw main stick
                 * (UCF reads it), the raw C-stick when there is one. */
                pad->button = (u16)((e[PRE_PHYS_BUTTONS] << 8) | e[PRE_PHYS_BUTTONS + 1]);
                pad->stickX = (s8)e[PRE_RAW_X];
                pad->stickY = len > PRE_RAW_Y ? (s8)e[PRE_RAW_Y] : 0;
                if (len > PRE_RAW_CY) {
                    P.raw_c = true;
                    pad->substickX = (s8)e[PRE_RAW_CX];
                    pad->substickY = (s8)e[PRE_RAW_CY];
                }
                fr->c[port][0] = bef(e + PRE_C_X);
                fr->c[port][1] = bef(e + PRE_C_Y);
                fr->cdir[port] = be32(e + PRE_BUTTONS) & C_DIRS;
                fr->pre_pos[port][0] = bef(e + PRE_X);
                fr->pre_pos[port][1] = bef(e + PRE_Y);
                fr->lr[port][0] = bef(e + PRE_PHYS_L);
                fr->lr[port][1] = bef(e + PRE_PHYS_R);
                fr->have[port] = true;
                P.present[port] = true;
                if (idx + 1 > P.count) {
                    P.count = idx + 1;
                }
            }
        } else if (cmd == SLP_CMD_PRE_FRAME && len > PRE_C_Y + 3 && e[PRE_FOLLOWER] && e[PRE_PORT] < 4) {
            /* a follower's comes after its leader's, which made room */
            const int32_t idx = (int32_t)be32(e + PRE_FRAME) - SLP_FIRST_FRAME;
            if (idx >= 0 && idx < cap) {
                PlayFrame* fr = &P.frames[idx];
                fr->fc[e[PRE_PORT]][0] = bef(e + PRE_C_X);
                fr->fc[e[PRE_PORT]][1] = bef(e + PRE_C_Y);
                fr->have_f[e[PRE_PORT]] = true;
            }
        } else if (cmd == SLP_CMD_POST_FRAME && len > POST_HITLAG + 3 && !e[PRE_FOLLOWER] && e[PRE_PORT] < 4) {
            /* the frame's pre-frame came first and made room for it */
            const int32_t idx = (int32_t)be32(e + PRE_FRAME) - SLP_FIRST_FRAME;
            if (idx >= 0 && idx < cap) {
                PlayFrame* fr = &P.frames[idx];
                const int port = e[PRE_PORT];
                fr->post_pos[port][0] = bef(e + POST_X);
                fr->post_pos[port][1] = bef(e + POST_Y);
                fr->hitlag[port] = bef(e + POST_HITLAG);
                fr->have_post[port] = true;
            }
        }
        e += len;
    }
    free(data);
    free(split);
    if (!have_start || P.count == 0) {
        pc_log_line("slp play: %s has no game start or no frames", path);
        return false;
    }
    P.last = SLP_FIRST_FRAME + P.count - 1;
    return true;
}

/* Once the pad library has its settings: every frame's normalized C-stick
 * and triggers back to raw values. */
static void convert(void) {
    const PadLibData* lib = &HSD_PadLibData;
    pc_log_line("slp play: pad library: clamp type %d shift %d min %d max %d, adc type %d threshold %d angle %.4f, "
                "stick scale %d, triggers clamp %d..%d shift %d scale %d",
        lib->clamp_stickType, lib->clamp_stickShift, lib->clamp_stickMin, lib->clamp_stickMax, lib->adc_type,
        lib->adc_th, lib->adc_angle, lib->scale_stick, lib->clamp_analogLRMin, lib->clamp_analogLRMax,
        lib->clamp_analogLRShift, lib->scale_analogLR);
    build_tables();
    for (int32_t i = 0; i < P.count; i++) {
        PlayFrame* fr = &P.frames[i];
        for (int port = 0; port < 4; port++) {
            if (!fr->have[port]) {
                continue;
            }
            PADStatus* pad = &fr->pad[port];
            if (!P.raw_c &&
                !c_raw(fr->c[port][0], fr->c[port][1], fr->cdir[port], -1, &pad->substickX, &pad->substickY))
            {
                if (P.c_misses++ < 5) {
                    pc_log_line("slp play: frame %d port %d: no raw C-stick gives %.4f,%.4f with directions %06x",
                        SLP_FIRST_FRAME + i, port, fr->c[port][0], fr->c[port][1], fr->cdir[port]);
                }
            }
            if (!lr_raw(fr->lr[port][0], &pad->triggerLeft) || !lr_raw(fr->lr[port][1], &pad->triggerRight)) {
                if (P.lr_misses++ < 5) {
                    pc_log_line("slp play: frame %d port %d: no raw trigger gives L %.4f R %.4f",
                        SLP_FIRST_FRAME + i, port, fr->lr[port][0], fr->lr[port][1]);
                }
            }
        }
    }
    if (!P.raw_c) {
        c_snaps_from_asdi();
        c_snaps_from_nana();
    }
    /* A frame a port's fighter sent nothing for (asleep, or out of the
     * match) keeps the pad it last had: the controller stays plugged in. */
    for (int32_t i = 1; i < P.count; i++) {
        for (int port = 0; port < 4; port++) {
            if (!P.frames[i].have[port] && P.present[port]) {
                P.frames[i].pad[port] = P.frames[i - 1].pad[port];
                P.frames[i].have[port] = P.frames[i - 1].have[port];
            }
        }
    }
    P.converted = true;
    pc_log_line("slp play: %d frames (%d to %d); C-stick %s, %u unmatched, %u snaps read from ASDI, %u from "
                "the follower; %u unmatched triggers", P.count, SLP_FIRST_FRAME, P.last,
        P.raw_c ? "raw" : "from its normalized values", P.c_misses, P.c_snaps_read, P.c_snaps_nana, P.lr_misses);
}

bool pc_slp_play_active(void) {
    if (P.state == 0) {
        const char* path = getenv("MELEE_SLP_PLAY");
        P.state = path != NULL && path[0] != '\0' && load(path) ? 1 : -1;
    }
    return P.state > 0;
}

uint32_t pc_slp_play_rng_offset(void) {
    return P.seed;
}

bool pc_slp_play_frozen_stadium(bool* frozen) {
    if (!pc_slp_play_active()) {
        return false;
    }
    *frozen = P.frozen_ps;
    return true;
}

#define BIT(v, n) (((v) >> (n)) & 1)

void pc_slp_start_from_info(struct StartMeleeData* start, const uint8_t in[SLP_INFO_BLOCK_SIZE]) {
    SlpInfoBlock b;
    slp_info_parse(in, &b);
    StartMeleeRules* r = &start->rules;
    r->match_kind = b.match_kind;
    r->x0_3 = b.x0_3;
    r->timer_enabled = b.timer_enabled;
    r->timer_counts_up = b.timer_counts_up;
    r->x1_0 = BIT(b.bits1, 7), r->x1_1 = BIT(b.bits1, 6), r->x1_2 = BIT(b.bits1, 5), r->x1_3 = BIT(b.bits1, 4);
    r->x1_4 = BIT(b.bits1, 3), r->x1_5 = BIT(b.bits1, 2), r->timer_shows_hours = BIT(b.bits1, 1);
    r->friendly_fire = BIT(b.bits1, 0);
    r->is_stock = BIT(b.bits2, 7), r->x2_1 = BIT(b.bits2, 6), r->x2_2 = BIT(b.bits2, 5);
    r->single_button = BIT(b.bits2, 4), r->disable_pausing = BIT(b.bits2, 3), r->x2_5 = BIT(b.bits2, 2);
    r->x2_6 = BIT(b.bits2, 1), r->x2_7 = BIT(b.bits2, 0);
    r->x3_0 = BIT(b.bits3, 7), r->x3_1 = BIT(b.bits3, 6), r->x3_2 = BIT(b.bits3, 5), r->x3_3 = BIT(b.bits3, 4);
    r->x3_4 = BIT(b.bits3, 3), r->x3_5 = BIT(b.bits3, 2), r->x3_6 = BIT(b.bits3, 1), r->x3_7 = BIT(b.bits3, 0);
    r->x4_0 = BIT(b.bits4, 7), r->is_vs = BIT(b.bits4, 6), r->x4_2 = BIT(b.bits4, 5), r->x4_3 = BIT(b.bits4, 4);
    r->x4_4 = BIT(b.bits4, 3), r->x4_5 = BIT(b.bits4, 2), r->x4_6 = BIT(b.bits4, 1), r->x4_7 = BIT(b.bits4, 0);
    r->x5_0 = BIT(b.bits5, 7), r->x5_1 = BIT(b.bits5, 6), r->x5_2 = BIT(b.bits5, 5), r->x5_3 = BIT(b.bits5, 4);
    r->x5_4 = BIT(b.bits5, 3), r->x5_5 = BIT(b.bits5, 2), r->x5_6 = BIT(b.bits5, 1), r->x5_7 = BIT(b.bits5, 0);
    r->x6 = b.x6;
    r->x7 = b.x7;
    r->is_teams = b.is_teams;
    r->x9 = b.x9;
    r->xA = b.xA;
    r->item_freq = b.item_freq;
    r->sd_penalty = b.sd_penalty;
    r->xD = b.xD;
    r->stkind = b.stkind;
    r->time_limit = b.time_limit;
    r->x14 = b.x14;
    r->x18 = b.x18;
    r->x1C_pad[0] = b.x1C;
    r->x20 = b.item_mask;
    r->x28 = b.x28;
    r->x2C = b.x2C;
    r->x30 = b.damage_ratio;
    r->game_speed = b.game_speed;
    for (int i = 0; i < 6; i++) {
        const SlpInfoPlayer* o = &b.players[i];
        PlayerInitData* p = &start->players[i];
        p->ckind = o->ckind;
        p->slot_type = o->slot_type;
        p->stocks = o->stocks;
        p->color = o->color;
        p->slot = o->slot;
        p->spawn_pos = o->spawn_pos;
        p->spawn_dir = o->spawn_dir;
        p->sub_color = o->sub_color;
        p->handicap = o->handicap;
        p->team = o->team;
        p->nametag = o->nametag;
        p->xB = o->xB;
        p->rumble_enabled = BIT(o->flags_c, 7), p->xC_b1 = BIT(o->flags_c, 6), p->vs_metal = BIT(o->flags_c, 5);
        p->xC_b3 = BIT(o->flags_c, 4), p->vs_invisible = BIT(o->flags_c, 3), p->xC_b5 = BIT(o->flags_c, 2);
        p->xC_b6 = BIT(o->flags_c, 1), p->xC_b7 = BIT(o->flags_c, 0);
        p->xD_b0 = BIT(o->flags_d, 7), p->xD_b1 = BIT(o->flags_d, 6), p->xD_b2 = BIT(o->flags_d, 5);
        p->xD_b3 = BIT(o->flags_d, 4), p->xD_b4 = BIT(o->flags_d, 3), p->xD_b5 = BIT(o->flags_d, 2);
        p->xD_b6 = BIT(o->flags_d, 1), p->xD_b7 = BIT(o->flags_d, 0);
        p->cpu_kind = o->cpu_kind;
        p->cpu_level = o->cpu_level;
        p->damage = o->damage;
        p->damage1 = o->damage1;
        p->hp = o->hp;
        p->attack_ratio = o->attack_ratio;
        p->defense_ratio = o->defense_ratio;
        p->model_scale = o->model_scale;
    }
}

void pc_slp_play_setup(struct StartMeleeData* start) {
    if (!pc_slp_play_active()) {
        return;
    }
    pc_slp_start_from_info(start, P.info);
    StartMeleeRules* r = &start->rules;
    /* A replay holds no paused frames (Slippi records fight frames only),
     * so the frame after a pause is the first one unpaused: played back
     * with pausing on, the Start that paused it would pause this game for
     * good, the next frame's pads never coming. */
    r->disable_pausing = true;
    pc_log_line("slp play: match set up from the replay (stage %u)", (unsigned)r->stkind);
}

void pc_slp_play_match_start(void) {
    if (pc_slp_play_active()) {
        P.started = true;
        P.next = SLP_FIRST_FRAME;
    }
}

void pc_slp_play_feed(PADStatus* head) {
    if (!pc_slp_play_active() || !P.started) {
        return;
    }
    if (!P.converted) {
        convert();
    }
    if (P.next > P.last + TAIL_FRAMES) {
        if (!pc_exit_requested) {
            pc_log_line("slp play: replay done at frame %d", P.next - 1);
            pc_exit_requested = true;
        }
        return;
    }
    /* The match's first frame starts from the agreed seed, as a netplay
     * session's does (net.c fresh_tick, at its start frame): the draws the
     * match setup made are behind it. In Slippi's terms, the rngOffset
     * InitOnlinePlay seeded. */
    if (P.next == SLP_FIRST_FRAME) {
        *HSD_RandSeedPtr = P.seed;
    }
    const int32_t idx = (P.next < P.last ? P.next : P.last) - SLP_FIRST_FRAME;
    const PlayFrame* fr = &P.frames[idx];
    for (int port = 0; port < 4; port++) {
        if (fr->have[port] || P.present[port]) {
            head[port] = fr->pad[port];
            head[port].err = PAD_ERR_NONE;
        } else {
            memset(&head[port], 0, sizeof head[port]);
            head[port].err = PAD_ERR_NO_CONTROLLER;
        }
    }
}

void pc_slp_play_match_end(void) {
    if (P.state > 0 && P.started && !pc_exit_requested) {
        pc_log_line("slp play: the match ended at frame %d (the replay's last is %d)", P.next - 1, P.last);
        pc_exit_requested = true;
    }
}

void pc_slp_play_tick_end(bool fight_frame) {
    if (P.state > 0 && P.started && fight_frame) {
        P.next++;
    }
}
