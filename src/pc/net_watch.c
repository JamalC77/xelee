/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Xelee Watch (net_watch.h; spectator-spec.md): a viewer headset runs the
 * host's VS match from the host's inputs, a short delay behind it.
 *
 * Host. When a VS match is set up (pc_watch_match_start, gmvs.c) with a
 * viewer subscribed, the match is shared: its header (everything the match
 * starts from, net_watch_wire.h) goes into a match log, and every tick
 * appends its four pads, checksum and seed (pc_watch_tx_frame, net.c). While
 * a shared match runs the host plays under the same pinned rules online
 * play uses (pc_net_deterministic), so both devices simulate alike. The
 * previous match's log is kept too, so a viewer that lags can finish it.
 *
 * Viewer. The lobby subscribes (HELLO, WELCOME), the watch thread stores
 * what arrives, and once a match's header is in the lobby leaves for it
 * (gmonlinemode.c). At that match's setup the header is written over the
 * game's start data, pads and seed; every tick then writes the next host
 * frame's pads and seed into the queue head (pc_watch_feed) and compares
 * its checksum with the host's (pc_watch_compare). A pass with no host frame
 * to run renders without ticking (pc_watch_hold, gmscene.c).
 *
 * One thread per process ("watch"), in either role, owns the socket: it
 * receives, acknowledges and resends on a 4 ms turn. The game thread never
 * touches the socket while that thread runs. It appends to or reads from the
 * logs under s_lock, which the thread holds only to copy, never while it
 * sends, so the host's frame never waits on a viewer.
 *
 * Milestone 1 (spectator-spec.md section 8.1): one viewer, an address given
 * by hand, no discovery beacon, no menu row.
 *
 * Knobs (melee-env.txt on a headset):
 *   MELEE_WATCH_SHARE=0|1      host: share matches (default on)
 *   MELEE_WATCH_PORT=n         host: stream port (default 41011)
 *   MELEE_WATCH_HOST=ip[:port] viewer: the host to watch
 *   MELEE_WATCH_BUFFER=n       viewer: frames in before the first tick (default 6)
 *   MELEE_WATCH_DUMP=path      host: also write every shared match to a file
 *   MELEE_WATCH_FILE=path      viewer: play such a file instead of a host
 *   MELEE_WATCH_TEST_WAIT=1    host: a match setup waits up to 10 s for a viewer
 *   MELEE_WATCH_DROP=pct       host: drop that share of FRAMES datagrams
 *   MELEE_WATCH_IGNORE_REV=1   both: accept another build */
#include "compat.h"
#include "pc/net_watch.h"
#include "pc/file_cache.h"
#include "pc/net.h"
#include "pc/net_lan.h"
#include "pc/net_watch_wire.h"
#include "pc/pc.h"
#include "pc/region.h"
#include "pc/slp.h"

#include <dolphin/pad.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wscalar-storage-order" /* disc-struct unions in lb/types.h */
#include <melee/ft/forward.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/types.h>
#include <melee/lb/lblanguage.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>
#pragma GCC diagnostic pop

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET watch_sock;
#define WATCH_SOCK_NONE INVALID_SOCKET
#define watch_sock_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int watch_sock;
#define WATCH_SOCK_NONE (-1)
#define watch_sock_close close
#endif

#define STREAM_PORT 41011
#define MAX_VIEWERS 1     /* milestone 1 */
#define VIEW_SLOTS 3      /* matches a viewer holds: the one playing, the next, a spare */
#define LOG_FRAMES0 36000 /* a host log's first allocation: ten minutes */
#define TURN_MS 4
#define MS_NS 1000000ull
#define HELLO_EVERY_NS (1000 * MS_NS)
#define REFUSED_HELLO_EVERY_NS (5000 * MS_NS)
#define HEADER_RESEND_NS (100 * MS_NS)
#define FRAMES_RESEND_NS (50 * MS_NS)
#define END_RESEND_NS (100 * MS_NS)
#define KEEPALIVE_NS (1000 * MS_NS)
#define ACK_MIN_NS (8 * MS_NS)
#define ACK_IDLE_NS (250 * MS_NS)
#define HOST_LOST_NS (5000 * MS_NS)
#define VIEWER_VERIFY_NS (3000 * MS_NS) /* a HELLO's slot until an ACK proves its address */
#define VIEWER_STALE_NS (2000 * MS_NS)  /* this quiet, a viewer has gone: its slot is free */
#define VIEWER_DROP_NS (10000 * MS_NS)
#define PREV_KEEP_NS (30000 * MS_NS)
#define TEST_WAIT_MS 10000
#define START_BUFFER 6
#define AHEAD_MAX 4096       /* frames a viewer stores past its contiguous run */
#define END_GRACE_PASSES 60  /* the host's match ended and ours has not asked to */
#define LEAVE_HOLD_PASSES 60 /* B held this long leaves the match */
#define REPORT_EVERY 600
#define TXQ_MAX 16
static const char k_dump_magic[8] = {'X', 'W', 'A', 'T', 'C', 'H', '1', '\n'};

_Static_assert(sizeof(float) == 4, "the nml floats travel as 32-bit patterns");
_Static_assert(offsetof(GameRules, unk_14) == WATCH_RULES_BYTES && sizeof(GameRules) == 0x18,
    "GameRules is twenty u8 fields and unk_14");
_Static_assert(PC_WATCH_INFO_SIZE == WATCH_INFO_SIZE, "the lobby's copy of the info block");
_Static_assert(PC_WATCH_NAME_LEN > WATCH_NAME_LEN, "a host's name fits the waiting screen's");

extern struct GameSceneInfo* gm_804D6720; /* current scene, gmscene.c */

static int watch_scene(void) {
    return gm_804D6720 != NULL ? gm_804D6720->scene_kind : -1;
}

/* ---- knobs --------------------------------------------------------------- */

static struct {
    bool read;
    int share; /* -1: the launcher decides */
    uint16_t port;
    const char* host; /* MELEE_WATCH_HOST */
    const char* dump;
    const char* file;
    int buffer;
    bool test_wait;
    int drop;
    bool ignore_rev;
    bool viewer_process; /* this process watches: it never shares */
} s_env;

static const char* env_str(const char* name) {
    const char* v = getenv(name);
    return v != NULL && v[0] != '\0' ? v : NULL;
}

static void env_read(void) {
    if (s_env.read) {
        return;
    }
    s_env.read = true;
    const char* v = env_str("MELEE_WATCH_SHARE");
    s_env.share = v == NULL ? -1 : v[0] != '0';
    v = env_str("MELEE_WATCH_PORT");
    s_env.port = v != NULL && atoi(v) > 0 && atoi(v) < 65536 ? (uint16_t)atoi(v) : STREAM_PORT;
    s_env.host = env_str("MELEE_WATCH_HOST");
    s_env.dump = env_str("MELEE_WATCH_DUMP");
    s_env.file = env_str("MELEE_WATCH_FILE");
    v = env_str("MELEE_WATCH_BUFFER");
    s_env.buffer = v != NULL && atoi(v) > 0 ? atoi(v) : START_BUFFER;
    v = env_str("MELEE_WATCH_TEST_WAIT");
    s_env.test_wait = v != NULL && v[0] == '1';
    v = env_str("MELEE_WATCH_DROP");
    s_env.drop = v != NULL ? atoi(v) : 0;
    v = env_str("MELEE_WATCH_IGNORE_REV");
    s_env.ignore_rev = v != NULL && v[0] == '1';
    v = env_str("MELEE_BOOT_SCENE");
    s_env.viewer_process =
        s_env.host != NULL || s_env.file != NULL || (v != NULL && strcmp(v, "watch") == 0);
}

/* ---- shared state -------------------------------------------------------- */

enum { ROLE_NONE, ROLE_HOST, ROLE_VIEWER };
enum { LINK_OFF, LINK_CONNECTING, LINK_WATCHING, LINK_REFUSED, LINK_LOST };

static int s_role; /* game thread; fixed while the thread runs */
static watch_sock s_sock = WATCH_SOCK_NONE;
static SDL_Thread* s_thread;
static atomic_bool s_run;
static SDL_Mutex* s_lock;
static atomic_bool s_tx_live;   /* host: a shared match is running */
static atomic_bool s_view_live; /* viewer: a watched match is running */
static atomic_bool s_pinned;    /* viewer: the host's rules are in force */
static atomic_int s_link = LINK_OFF;
/* This build, this disc, this device: copied on the game thread before the
 * thread starts, so the thread never calls into the launcher or the DVD. */
static char s_my_rev[WATCH_REV_LEN + 1];
static char s_my_disc[WATCH_DISC_LEN + 1];
static char s_my_name[WATCH_NAME_LEN + 1];

static void watch_lock(void) {
    if (s_lock != NULL) {
        SDL_LockMutex(s_lock);
    }
}

static void watch_unlock(void) {
    if (s_lock != NULL) {
        SDL_UnlockMutex(s_lock);
    }
}

static uint64_t now_ns(void) {
    return SDL_GetTicksNS();
}

/* Not a secret, only unpredictable enough that a stray datagram does not
 * guess a live cookie (spectator-spec.md section 2.4). */
static uint32_t mix32(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (uint32_t)x | 1u;
}

static uint32_t new_token(uint64_t salt) {
    static uint64_t counter;
    counter++;
    return mix32(SDL_GetPerformanceCounter() ^ salt ^ (counter << 40));
}

/* ---- sockets ------------------------------------------------------------- */

static void net_startup(void) {
#if defined(_WIN32)
    static bool done;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
#endif
}

static bool sock_nonblocking(watch_sock s) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    int f = fcntl(s, F_GETFL, 0);
    return f != -1 && fcntl(s, F_SETFL, f | O_NONBLOCK) != -1;
#endif
}

/* UDP on 0.0.0.0:port (0: any), non-blocking. */
static watch_sock open_socket(uint16_t port) {
    watch_sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == WATCH_SOCK_NONE) {
        return s;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (const struct sockaddr*)&a, sizeof a) != 0 || !sock_nonblocking(s)) {
        watch_sock_close(s);
        return WATCH_SOCK_NONE;
    }
    return s;
}

static uint16_t socket_port(watch_sock s) {
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(s, (struct sockaddr*)&a, &len) != 0) {
        return 0;
    }
    return ntohs(a.sin_port);
}

/* Block until the socket has a datagram or ms pass. select() is Winsock's
 * wait; a POSIX descriptor can be past FD_SETSIZE, so poll() there. */
static void wait_readable(watch_sock s, int ms) {
#if defined(_WIN32)
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    struct timeval tv = {0, ms * 1000};
    select(0, &r, NULL, NULL, &tv);
#else
    struct pollfd p = {s, POLLIN, 0};
    poll(&p, 1, ms);
#endif
}

static void send_to(const struct sockaddr_in* to, const uint8_t* buf, size_t n) {
    if (s_sock != WATCH_SOCK_NONE && n > 0) {
        sendto(s_sock, (const char*)buf, (int)n, 0, (const struct sockaddr*)to, sizeof *to);
    }
}

static bool same_addr(const struct sockaddr_in* a, const struct sockaddr_in* b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void addr_text(const struct sockaddr_in* a, char* out, size_t cap) {
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    snprintf(out, cap, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
}

/* This device's IPv4 on the route out (a connected, never sent on, UDP
 * socket: no packet leaves), for the line a viewer's address is read from. */
static void local_ipv4(char* out, size_t cap) {
    static const char* const probes[] = {"8.8.8.8", "224.0.0.251"};
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        struct sockaddr_in to, self;
        socklen_t len = sizeof self;
        memset(&to, 0, sizeof to);
        memset(&self, 0, sizeof self);
        to.sin_family = AF_INET;
        to.sin_port = htons(53);
        if (inet_pton(AF_INET, probes[i], &to.sin_addr) != 1) {
            continue;
        }
        watch_sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == WATCH_SOCK_NONE) {
            continue;
        }
        bool ok = connect(s, (const struct sockaddr*)&to, sizeof to) == 0 &&
                  getsockname(s, (struct sockaddr*)&self, &len) == 0 &&
                  self.sin_addr.s_addr != htonl(INADDR_ANY);
        watch_sock_close(s);
        if (ok && inet_ntop(AF_INET, &self.sin_addr, out, (socklen_t)cap) != NULL) {
            return;
        }
    }
    snprintf(out, cap, "unknown");
}

/* Datagrams the thread queues under s_lock and sends after releasing it. */
static uint8_t s_txq[TXQ_MAX][WATCH_DATAGRAM_MAX];
static size_t s_txq_len[TXQ_MAX];
static struct sockaddr_in s_txq_to[TXQ_MAX];
static int s_txq_n;

/* Room for the next datagram, NULL when the queue is full (what does not fit
 * goes out next turn). */
static uint8_t* txq_slot(void) {
    return s_txq_n < TXQ_MAX ? s_txq[s_txq_n] : NULL;
}

static void txq_push(const struct sockaddr_in* to, size_t n) {
    if (n > 0 && s_txq_n < TXQ_MAX) {
        s_txq_len[s_txq_n] = n;
        s_txq_to[s_txq_n] = *to;
        s_txq_n++;
    }
}

static void txq_flush(void) {
    for (int i = 0; i < s_txq_n; i++) {
        send_to(&s_txq_to[i], s_txq[i], s_txq_len[i]);
    }
    s_txq_n = 0;
}

/* ---- pads ---------------------------------------------------------------- */

static void pad_to_wire(WatchPad* w, const PADStatus* p) {
    w->button = p->button;
    w->stick_x = p->stickX;
    w->stick_y = p->stickY;
    w->substick_x = p->substickX;
    w->substick_y = p->substickY;
    w->trigger_l = p->triggerLeft;
    w->trigger_r = p->triggerRight;
    w->analog_a = p->analogA;
    w->analog_b = p->analogB;
    w->err = p->err;
}

/* A PADStatus from the wire, its padding zeroed as the host's is (below). */
static void pad_from_wire(PADStatus* out, const WatchPad* w) {
    PADStatus p;
    memset(&p, 0, sizeof p);
    p.button = w->button;
    p.stickX = w->stick_x;
    p.stickY = w->stick_y;
    p.substickX = w->substick_x;
    p.substickY = w->substick_y;
    p.triggerLeft = w->trigger_l;
    p.triggerRight = w->trigger_r;
    p.analogA = w->analog_a;
    p.analogB = w->analog_b;
    p.err = w->err;
    memcpy(out, &p, sizeof p);
}

/* frame_checksum (net_snapshot.c) hashes each PADStatus whole, the padding
 * after err included, and what that byte holds depends on how the sample
 * was made. The record carries the fields only, so both sides zero it. */
static void pad_clear_padding(PADStatus* p) {
    const size_t end = offsetof(PADStatus, err) + sizeof p->err;
    memset((uint8_t*)p + end, 0, sizeof *p - end);
}

static void pad_status_to_wire(WatchPadStatus* w, const HSD_PadStatus* s) {
    const float nml[8] = {s->nml_stickX, s->nml_stickY, s->nml_subStickX, s->nml_subStickY,
        s->nml_analogL, s->nml_analogR, s->nml_analogA, s->nml_analogB};
    w->button = s->button;
    w->last_button = s->last_button;
    w->trigger = s->trigger;
    w->repeat = s->repeat;
    w->release = s->release;
    w->repeat_count = s->repeat_count;
    w->stick_x = s->stickX;
    w->stick_y = s->stickY;
    w->substick_x = s->subStickX;
    w->substick_y = s->subStickY;
    w->analog_l = s->analogL;
    w->analog_r = s->analogR;
    w->analog_a = s->analogA;
    w->analog_b = s->analogB;
    memcpy(w->nml, nml, sizeof w->nml);
    w->cross_dir = s->cross_dir;
    w->err = s->err;
}

static void pad_status_from_wire(HSD_PadStatus* s, const WatchPadStatus* w) {
    float nml[8];
    memcpy(nml, w->nml, sizeof nml);
    s->button = w->button;
    s->last_button = w->last_button;
    s->trigger = w->trigger;
    s->repeat = w->repeat;
    s->release = w->release;
    s->repeat_count = w->repeat_count;
    s->stickX = w->stick_x;
    s->stickY = w->stick_y;
    s->subStickX = w->substick_x;
    s->subStickY = w->substick_y;
    s->analogL = w->analog_l;
    s->analogR = w->analog_r;
    s->analogA = w->analog_a;
    s->analogB = w->analog_b;
    s->nml_stickX = nml[0];
    s->nml_stickY = nml[1];
    s->nml_subStickX = nml[2];
    s->nml_subStickY = nml[3];
    s->nml_analogL = nml[4];
    s->nml_analogR = nml[5];
    s->nml_analogA = nml[6];
    s->nml_analogB = nml[7];
    s->cross_dir = w->cross_dir;
    s->err = w->err;
}

/* Pokemon Stadium's transformations load mid-match, served from memory on
 * both devices while the match is shared (pc_net_pure_load); fetch them now
 * so the first one is no hitch. */
static void stadium_prefetch(int stkind) {
    static const char* const files[] = {"GrPs1.dat", "GrPs2.dat", "GrPs3.dat", "GrPs4.dat"};
    if (stkind != St_Kind_PStadium) {
        return;
    }
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        if (!pc_file_cache_require(files[i])) {
            pc_log_line("watch: %s could not be cached; its load will hitch", files[i]);
        }
    }
}

static const char* mode_name(int mode) {
    switch (mode) {
    case GM_CLASSIC:
        return "Classic";
    case GM_ADVENTURE:
        return "Adventure";
    case GM_ALLSTAR:
        return "All-Star";
    case GM_TRAINING:
        return "Training";
    case GM_EVENT:
        return "Event Match";
    case GM_HOME_RUN_CONTEST:
        return "Home-Run Contest";
    case GM_TARGET_TEST:
        return "Target Test";
    case GM_10MAN_VS:
    case GM_100MAN_VS:
    case GM_3MIN_VS:
    case GM_15MIN_VS:
    case GM_ENDLESS_VS:
    case GM_CRUEL_VS:
        return "Multi-Man Melee";
    case GM_TOURNAMENT:
        return "Tournament";
    case GM_SUPER_SUDDEN_DEATH_VS:
    case GM_INVISIBLE_VS:
    case GM_SLOMO_VS:
    case GM_LIGHTNING_VS:
    case GM_TINY_VS:
    case GM_GIANT_VS:
    case GM_STAMINA_VS:
    case GM_CAMERA_VS:
    case GM_SINGLE_BUTTON_VS:
        return "Special Melee";
    default:
        return "another mode";
    }
}

/* A header's stage and the characters of its four ports (-1: none), from
 * its Game Info Block (slp_format.c's layout). */
static unsigned info_stage(const uint8_t* info) {
    return (unsigned)(info[0xE] << 8 | info[0xF]);
}

static int info_char(const uint8_t* info, int port) {
    const uint8_t* p = info + 0x60 + 0x24 * port;
    return p[1] != 3 ? (int)(int8_t)p[0] : -1; /* slot type 3: nobody */
}

/* Why a header's stage or players are not ones this game has (a damaged
 * dump, or another build under MELEE_WATCH_IGNORE_REV), NULL when they are.
 * The match setup indexes the character, costume and stage tables with
 * them, and the forced stage select takes the stage as an s8. */
static const char* info_unplayable(const uint8_t* info) {
    const unsigned stage = info_stage(info);
    if (stage <= St_Kind_Test || stage > St_Kind_Heal) {
        return "an unknown stage";
    }
    for (int port = 0; port < 6; port++) {
        const uint8_t* p = info + 0x60 + 0x24 * port;
        const uint8_t ckind = p[0], slot_type = p[1], color = p[3];
        if (slot_type == Gm_PKind_NA) {
            continue;
        }
        if (slot_type > Gm_PKind_Boss) {
            return "an unknown player kind";
        }
        if (ckind >= CKind_Playable_Count) {
            return "an unknown character";
        }
        if (color >= gm_GetNumCostumesForCKind(ckind)) {
            return "a costume its character does not have";
        }
    }
    return NULL;
}

static void log_match(const char* what, const WatchHeader* h) {
    pc_log_line("watch: %s match %u: mode %u scene %u stage %u chars %d/%d/%d/%d seed %08x first "
                "frame %d; ucf %u fixes %u frozen %u unlock all %u (%016llx); rev %s",
        what, (unsigned)h->match_seq, (unsigned)h->host_mode, (unsigned)h->scene_kind,
        info_stage(h->info), info_char(h->info, 0), info_char(h->info, 1), info_char(h->info, 2),
        info_char(h->info, 3), (unsigned)h->seed, (int)h->first_frame, (unsigned)h->ucf,
        (unsigned)h->slippi_fixes, (unsigned)h->frozen_stadium, (unsigned)h->unlock_all,
        (unsigned long long)h->unlock_state, h->rev);
}

/* ---- host ---------------------------------------------------------------- */

/* One viewer's progress through one match log. */
typedef struct LogViewer {
    bool attached; /* subscribed when the match started: the match is for it */
    bool header_ok, end_ok;
    int32_t contiguous; /* newest frame it holds with every one before it */
    int32_t sent_upto;  /* newest frame a FRAMES datagram has carried */
    uint64_t header_ns, frames_ns, end_ns;
} LogViewer;

typedef struct MatchLog {
    bool used;
    uint32_t seq;
    uint8_t header[WATCH_HEADER_SIZE];
    uint8_t* frames; /* count packed WatchFrames */
    int32_t count, cap;
    bool ended;
    int32_t last;
    uint8_t end_reason;
    uint64_t ended_ns;
    LogViewer vw[MAX_VIEWERS];
} MatchLog;

/* A HELLO's source address can be forged, so a slot gets nothing but the
 * WELCOME until an ACK carrying its cookie comes back from that address
 * (spectator-spec.md section 2.4): no STATUS, no HEADER, no match, and it
 * neither keeps the slot past VIEWER_VERIFY_NS nor stops another viewer
 * taking it. Only such ACKs keep a slot alive. */
typedef struct Viewer {
    bool used;
    bool verified; /* an ACK with its cookie came from its address */
    struct sockaddr_in addr;
    uint64_t id;
    uint32_t cookie;
    uint32_t nonce; /* its HELLO's: a repeat of it gets the same WELCOME */
    char name[WATCH_NAME_LEN + 1];
    char rev[WATCH_REV_LEN + 1];
    uint64_t heard_ns, status_ns;
} Viewer;

/* Under s_lock: */
static MatchLog s_logs[2]; /* the current match and the previous one */
static int s_cur;          /* index of the current one */
static Viewer s_viewers[MAX_VIEWERS];
static uint8_t s_state = WATCH_STATE_MENUS;
static uint8_t s_mode;
static uint32_t s_live_seq; /* the match STATUS reports */
static bool s_status_dirty;
static uint64_t s_tx_bytes;
static unsigned s_tx_resends;
static uint32_t s_drop_rng = 0x2545F491u;

/* Game thread: */
static bool s_share = true; /* the launcher's setting */
static bool s_host_failed;  /* the stream port did not open */
static uint32_t s_seq;      /* the newest watchable match's number */
static int32_t s_tx_first;  /* the host's frame number of the match's first tick */
static int s_tx_scene;
static bool s_tx_gap_logged;
static FILE* s_dump;
static bool s_dump_failed;
static uint64_t s_report_ns;
static uint64_t s_report_bytes;

static void log_free(MatchLog* l) {
    free(l->frames);
    memset(l, 0, sizeof *l);
}

/* Viewers a match starting now would be shared with. */
static int count_viewers(void) {
    int n = 0;
    watch_lock();
    for (int i = 0; i < MAX_VIEWERS; i++) {
        n += s_viewers[i].used && s_viewers[i].verified;
    }
    watch_unlock();
    return n;
}

static void host_set_state(uint8_t state, int mode) {
    watch_lock();
    if (s_state != state || s_mode != (uint8_t)mode) {
        s_status_dirty = true;
    }
    s_state = state;
    s_mode = (uint8_t)mode;
    watch_unlock();
}

static bool drop_this(void) {
    if (s_env.drop <= 0) {
        return false;
    }
    s_drop_rng ^= s_drop_rng << 13;
    s_drop_rng ^= s_drop_rng >> 17;
    s_drop_rng ^= s_drop_rng << 5;
    return (int)(s_drop_rng % 100u) < s_env.drop;
}

/* Thread, under s_lock: what a viewer of this log needs next. */
static void serve_log(MatchLog* l, int slot, const Viewer* v, uint64_t now) {
    LogViewer* lv = &l->vw[slot];
    if (!lv->attached) {
        return;
    }
    if (!lv->header_ok) {
        if (now - lv->header_ns >= HEADER_RESEND_NS) {
            lv->header_ns = now;
            const int cnt = watch_chunk_count(WATCH_HEADER_SIZE);
            for (int i = 0; i < cnt; i++) {
                uint8_t* out = txq_slot();
                if (out == NULL) {
                    break;
                }
                size_t n = watch_pack_chunk(
                    out, WATCH_DATAGRAM_MAX, v->cookie, l->seq, l->header, WATCH_HEADER_SIZE, i);
                s_tx_bytes += n;
                txq_push(&v->addr, n);
            }
        }
        return; /* frames once it has the header */
    }
    const int32_t first = lv->contiguous + 1;
    const int32_t avail = l->count - first;
    if (avail > 0) {
        const int32_t cnt = avail < WATCH_FRAMES_MAX ? avail : WATCH_FRAMES_MAX;
        const bool fresh = first + cnt - 1 > lv->sent_upto;
        uint8_t* out = txq_slot();
        if (out != NULL && (fresh || now - lv->frames_ns >= FRAMES_RESEND_NS)) {
            if (!fresh) {
                s_tx_resends++;
            }
            WatchFrames m = {v->cookie, l->seq, first, (uint8_t)cnt,
                l->frames + (size_t)first * WATCH_FRAME_SIZE};
            if (!drop_this()) {
                size_t n = watch_pack_frames(out, WATCH_DATAGRAM_MAX, &m);
                s_tx_bytes += n;
                txq_push(&v->addr, n);
            }
            if (first + cnt - 1 > lv->sent_upto) {
                lv->sent_upto = first + cnt - 1;
            }
            lv->frames_ns = now;
        }
    }
    if (l->ended && !lv->end_ok && now - lv->end_ns >= END_RESEND_NS) {
        uint8_t* out = txq_slot();
        if (out != NULL) {
            WatchMatchEnd e = {v->cookie, l->seq, l->last, l->end_reason};
            size_t n = watch_pack_match_end(out, WATCH_DATAGRAM_MAX, &e);
            s_tx_bytes += n;
            txq_push(&v->addr, n);
            lv->end_ns = now;
        }
    }
}

static void host_turn(uint64_t now) {
    watch_lock();
    for (int i = 0; i < MAX_VIEWERS; i++) {
        Viewer* v = &s_viewers[i];
        if (v->used && now - v->heard_ns > (v->verified ? VIEWER_DROP_NS : VIEWER_VERIFY_NS)) {
            if (v->verified) {
                char at[32];
                addr_text(&v->addr, at, sizeof at);
                pc_log_line("watch: viewer %s (%s) dropped: silent for 10 s", v->name, at);
            }
            v->used = false;
            v->verified = false;
            for (int k = 0; k < 2; k++) {
                memset(&s_logs[k].vw[i], 0, sizeof s_logs[k].vw[i]);
            }
        }
    }
    /* The previous match is kept until every viewer it was for has all of
     * it, or 30 s: a viewer that lags can finish it. */
    MatchLog* prev = &s_logs[1 - s_cur];
    if (prev->used && prev->ended) {
        bool done = true;
        for (int i = 0; i < MAX_VIEWERS; i++) {
            const LogViewer* lv = &prev->vw[i];
            if (lv->attached && !(lv->end_ok && lv->contiguous >= prev->last)) {
                done = false;
            }
        }
        if (done || now - prev->ended_ns > PREV_KEEP_NS) {
            log_free(prev);
        }
    }
    const MatchLog* cur = &s_logs[s_cur];
    const int32_t live_frame =
        s_state == WATCH_STATE_LIVE && cur->used && cur->seq == s_live_seq ? cur->count - 1 : -1;
    for (int i = 0; i < MAX_VIEWERS; i++) {
        Viewer* v = &s_viewers[i];
        if (!v->used || !v->verified) {
            continue;
        }
        if (s_status_dirty || now - v->status_ns >= KEEPALIVE_NS) {
            uint8_t* out = txq_slot();
            if (out != NULL) {
                WatchStatus st = {v->cookie, s_state, s_mode, s_live_seq, live_frame};
                size_t n = watch_pack_status(out, WATCH_DATAGRAM_MAX, &st);
                s_tx_bytes += n;
                txq_push(&v->addr, n);
                v->status_ns = now;
            }
        }
        /* The older match first: it is the one the viewer plays first. */
        if (s_logs[1 - s_cur].used) {
            serve_log(&s_logs[1 - s_cur], i, v, now);
        }
        if (s_logs[s_cur].used) {
            serve_log(&s_logs[s_cur], i, v, now);
        }
    }
    s_status_dirty = false;
    watch_unlock();
    txq_flush();
}

/* Under s_lock: the subscribed viewer a cookie and address belong to. */
static Viewer* viewer_by_cookie(uint32_t cookie, const struct sockaddr_in* from, int* slot) {
    for (int i = 0; i < MAX_VIEWERS; i++) {
        Viewer* v = &s_viewers[i];
        if (v->used && v->cookie == cookie && same_addr(&v->addr, from)) {
            *slot = i;
            return v;
        }
    }
    return NULL;
}

static void host_on_hello(
    const uint8_t* buf, size_t n, const struct sockaddr_in* from, uint64_t now) {
    WatchHello h;
    if (!watch_unpack_hello(buf, n, &h)) {
        return;
    }
    uint8_t reason = WATCH_REFUSE_NONE;
    if (!s_env.ignore_rev && strncmp(h.rev, s_my_rev, WATCH_REV_LEN) != 0) {
        reason = WATCH_REFUSE_REV;
    } else if (strcmp(h.disc, s_my_disc) != 0) {
        reason = WATCH_REFUSE_DISC;
    }
    char at[32];
    addr_text(from, at, sizeof at);
    WatchWelcome w;
    memset(&w, 0, sizeof w);
    w.nonce = h.nonce;
    snprintf(w.name, sizeof w.name, "%s", s_my_name);
    char gone_name[WATCH_NAME_LEN + 1] = "";
    char gone_at[32] = "";
    watch_lock();
    int slot = -1;
    for (int i = 0; i < MAX_VIEWERS && slot < 0; i++) {
        if (s_viewers[i].used && s_viewers[i].id == h.viewer_id) {
            slot = i; /* the same viewer again: it lost our WELCOME, or the host */
        }
    }
    for (int i = 0; i < MAX_VIEWERS && slot < 0; i++) {
        if (!s_viewers[i].used) {
            slot = i;
        }
    }
    /* A slot nobody holds: a HELLO's that never proved its address, or a
     * viewer gone quiet past its 250 ms ACKs without a BYE (closed, killed,
     * or back on a new port, so with a new id). */
    for (int i = 0; i < MAX_VIEWERS && slot < 0; i++) {
        const Viewer* v = &s_viewers[i];
        if (!v->verified || now - v->heard_ns > VIEWER_STALE_NS) {
            slot = i;
        }
    }
    if (reason == WATCH_REFUSE_NONE && slot < 0) {
        reason = WATCH_REFUSE_FULL;
    }
    if (reason == WATCH_REFUSE_NONE) {
        Viewer* v = &s_viewers[slot];
        if (!(v->used && v->nonce == h.nonce && same_addr(&v->addr, from))) {
            /* A new subscription: no match already running is for it. */
            if (v->used && v->verified && v->id != h.viewer_id) {
                snprintf(gone_name, sizeof gone_name, "%s", v->name);
                addr_text(&v->addr, gone_at, sizeof gone_at);
            }
            v->used = true;
            v->verified = false;
            v->addr = *from;
            v->id = h.viewer_id;
            v->nonce = h.nonce;
            v->cookie = new_token(h.viewer_id ^ h.nonce);
            snprintf(v->name, sizeof v->name, "%s", h.name);
            snprintf(v->rev, sizeof v->rev, "%s", h.rev);
            v->heard_ns = now; /* only its ACKs keep it from here */
            for (int k = 0; k < 2; k++) {
                memset(&s_logs[k].vw[slot], 0, sizeof s_logs[k].vw[slot]);
            }
        }
        w.ok = 1;
        w.cookie = v->cookie;
    }
    w.reason = reason;
    w.state = s_state;
    w.mode = s_mode;
    watch_unlock();
    uint8_t out[WATCH_WELCOME_SIZE];
    send_to(from, out, watch_pack_welcome(out, sizeof out, &w));
    if (gone_name[0] != '\0' || gone_at[0] != '\0') {
        pc_log_line("watch: viewer %s (%s) dropped: silent, and %s (%s) asked for its slot",
            gone_name, gone_at, h.name, at);
    }
    if (reason != WATCH_REFUSE_NONE) {
        /* A viewer's repeats of one HELLO once, and at most one line per
         * few seconds whatever the nonces: HELLOs are cheap to send, and
         * the log is a file on the headset. */
        static uint32_t logged_nonce;
        static uint64_t logged_ns;
        static unsigned suppressed;
        if (h.nonce != logged_nonce && logged_ns != 0 &&
            now - logged_ns < REFUSED_HELLO_EVERY_NS)
        {
            suppressed++;
        } else if (h.nonce != logged_nonce) {
            char more[48] = "";
            if (suppressed > 0) {
                snprintf(more, sizeof more, "; %u other refusals before it", suppressed);
            }
            logged_nonce = h.nonce;
            logged_ns = now;
            suppressed = 0;
            pc_log_line("watch: refused %s (%s): %s (theirs rev %s disc %s, ours rev %s disc %s)%s",
                h.name, at,
                reason == WATCH_REFUSE_REV  ? "another build" :
                reason == WATCH_REFUSE_DISC ? "another disc image" :
                                              "no free viewer slot",
                h.rev, h.disc, s_my_rev, s_my_disc, more);
        }
    }
}

static void host_on_ack(
    const uint8_t* buf, size_t n, const struct sockaddr_in* from, uint64_t now) {
    WatchAck a;
    if (!watch_unpack_ack(buf, n, &a)) {
        return;
    }
    char name[WATCH_NAME_LEN + 1] = "";
    char rev[WATCH_REV_LEN + 1] = "";
    bool verified_now = false;
    watch_lock();
    int slot = 0;
    Viewer* v = viewer_by_cookie(a.cookie, from, &slot);
    if (v != NULL) {
        v->heard_ns = now;
        if (!v->verified) {
            /* Its address is its own: from here it is served. */
            v->verified = true;
            verified_now = true;
            v->status_ns = 0;
            snprintf(name, sizeof name, "%s", v->name);
            snprintf(rev, sizeof rev, "%s", v->rev);
        }
        for (int k = 0; k < 2; k++) {
            MatchLog* l = &s_logs[k];
            LogViewer* lv = &l->vw[slot];
            if (!l->used || l->seq != a.match_seq || !lv->attached) {
                continue;
            }
            if (a.flags & WATCH_ACK_HEADER) {
                lv->header_ok = true;
            }
            if (a.flags & WATCH_ACK_END) {
                lv->end_ok = true;
            }
            if (a.contiguous > lv->contiguous && a.contiguous < l->count) {
                lv->contiguous = a.contiguous;
            }
        }
    }
    watch_unlock();
    if (verified_now) {
        char at[32];
        addr_text(from, at, sizeof at);
        pc_log_line("watch: viewer %s (%s) subscribed, rev %s disc %s", name, at, rev, s_my_disc);
    }
}

static void host_on_bye(const uint8_t* buf, size_t n, const struct sockaddr_in* from) {
    uint32_t cookie;
    if (!watch_unpack_bye(buf, n, &cookie)) {
        return;
    }
    char name[WATCH_NAME_LEN + 1] = "";
    watch_lock();
    int slot = 0;
    Viewer* v = viewer_by_cookie(cookie, from, &slot);
    if (v != NULL) {
        snprintf(name, sizeof name, "%s", v->name);
        v->used = false;
        v->verified = false;
        for (int k = 0; k < 2; k++) {
            memset(&s_logs[k].vw[slot], 0, sizeof s_logs[k].vw[slot]);
        }
    }
    watch_unlock();
    if (v != NULL) {
        char at[32];
        addr_text(from, at, sizeof at);
        pc_log_line("watch: viewer %s (%s) left", name, at);
    }
}

static void host_on_datagram(
    const uint8_t* buf, size_t n, const struct sockaddr_in* from, uint64_t now) {
    switch (watch_msg_type(buf, n)) {
    case WATCH_MSG_HELLO:
        host_on_hello(buf, n, from, now);
        break;
    case WATCH_MSG_ACK:
        host_on_ack(buf, n, from, now);
        break;
    case WATCH_MSG_BYE:
        host_on_bye(buf, n, from);
        break;
    case WATCH_MSG_OTHER_PROTO: {
        static bool logged;
        if (!logged) {
            char at[32];
            addr_text(from, at, sizeof at);
            logged = true;
            pc_log_line("watch: %s speaks another watch protocol (ours is %d)", at, WATCH_PROTO);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- viewer -------------------------------------------------------------- */

/* One host match as it arrives: its header, its frames, its end. */
typedef struct ViewMatch {
    bool used;
    uint32_t seq;
    WatchAssembly hdr;
    bool have_header;
    bool refused; /* complete, but not a match this build can play */
    WatchHeader header;
    uint64_t header_ns; /* when the header became complete */
    uint8_t* frames;    /* cap packed WatchFrames */
    uint8_t* have;      /* cap flags */
    int32_t cap;
    int32_t contiguous; /* newest frame held with every one before it */
    bool end_known;
    int32_t last;
    uint8_t end_reason;
    bool dirty;    /* something arrived since the last ACK */
    bool orphaned; /* subscribed again mid-match: the host serves it no more */
    bool taken, played;
    unsigned packets, gaps;
} ViewMatch;

/* Under s_lock: */
static ViewMatch s_vm[VIEW_SLOTS];
static int s_play = -1;  /* the slot being played */
static int s_taken = -1; /* the slot the lobby took, not started yet */
static struct sockaddr_in s_host_addr;
static char s_host_text[32]; /* ip:port */
static char s_host_name[WATCH_NAME_LEN + 1];
static uint32_t s_cookie, s_nonce;
static uint8_t s_refused;
static WatchStatus s_host_status;
static uint64_t s_status_ns; /* when s_host_status's frame arrived */
static bool s_have_status;
static uint64_t s_hello_ns, s_heard_ns, s_ack_ns;
static uint64_t s_my_id;
static atomic_int s_next_pub; /* s_next, for the ACK's buffered field */

/* Game thread: */
static bool s_file_mode, s_file_opened, s_file_done;
static FILE* s_file;
static WatchHeader s_taken_hdr; /* the taken match's header, for the pin and the setup */
static int32_t s_next;          /* the next match frame to run */
static bool s_started;          /* the start buffer filled */
static bool s_have_cur;
static uint32_t s_cur_ck;
static int32_t s_cur_frame;
static unsigned s_mismatches;
static bool s_end_asked;
static int32_t s_end_asked_at;
static int s_end_wait;
static int s_leave_passes;
static bool s_leaving;
static uint32_t s_last_buttons;
static bool s_after_match, s_last_diverged;
static unsigned s_matches_done;
/* the 600-frame report */
static unsigned s_starved;
static int32_t s_buf_min, s_buf_max;
static int64_t s_buf_sum;
static int s_buf_n;

static void vm_free(ViewMatch* vm) {
    free(vm->frames);
    free(vm->have);
    memset(vm, 0, sizeof *vm);
}

static void vm_init(ViewMatch* vm, uint32_t seq) {
    vm_free(vm);
    vm->used = true;
    vm->seq = seq;
    watch_assembly_reset(&vm->hdr, seq);
    vm->contiguous = -1;
    vm->last = -1;
}

/* Under s_lock: the slot for match seq, made if `create`: a free one, else
 * one already played, else the oldest; never the one playing or taken.
 * NULL when there is none. */
static ViewMatch* vm_for(uint32_t seq, bool create) {
    for (int i = 0; i < VIEW_SLOTS; i++) {
        /* An orphaned match is another subscription's: its number may come
         * round again from a host that restarted. */
        if (s_vm[i].used && !s_vm[i].orphaned && s_vm[i].seq == seq) {
            return &s_vm[i];
        }
    }
    if (!create) {
        return NULL;
    }
    int pick = -1;
    for (int i = 0; i < VIEW_SLOTS; i++) {
        if (i == s_play || i == s_taken) {
            continue;
        }
        if (!s_vm[i].used) {
            pick = i;
            break;
        }
        if (pick < 0 || (s_vm[i].played && !s_vm[pick].played) ||
            (s_vm[i].played == s_vm[pick].played && s_vm[i].seq < s_vm[pick].seq))
        {
            pick = i;
        }
    }
    if (pick < 0) {
        return NULL;
    }
    if (s_vm[pick].used && !s_vm[pick].played) {
        pc_log_line("watch: match %u dropped unplayed: match %u arrived", (unsigned)s_vm[pick].seq,
            (unsigned)seq);
    }
    vm_init(&s_vm[pick], seq);
    return &s_vm[pick];
}

/* Under s_lock: a frame record into match vm. */
static void vm_store(ViewMatch* vm, int32_t f, const uint8_t* rec) {
    if (f <= vm->contiguous || f > vm->contiguous + AHEAD_MAX || (vm->end_known && f > vm->last)) {
        return;
    }
    if (f >= vm->cap) {
        int32_t cap = vm->cap > 0 ? vm->cap : 4096;
        while (cap <= f) {
            cap *= 2;
        }
        uint8_t* frames = realloc(vm->frames, (size_t)cap * WATCH_FRAME_SIZE);
        if (frames == NULL) {
            return;
        }
        vm->frames = frames;
        uint8_t* have = realloc(vm->have, (size_t)cap);
        if (have == NULL) {
            return;
        }
        memset(have + vm->cap, 0, (size_t)(cap - vm->cap));
        vm->have = have;
        vm->cap = cap;
    }
    if (!vm->have[f]) {
        memcpy(vm->frames + (size_t)f * WATCH_FRAME_SIZE, rec, WATCH_FRAME_SIZE);
        vm->have[f] = 1;
    }
    while (vm->contiguous + 1 < vm->cap && vm->have[vm->contiguous + 1]) {
        vm->contiguous++;
    }
}

/* Under s_lock: a header just completed. Refused when it is not a VS match
 * of this build and disc: the two devices would not run the same code. */
static void vm_header_done(ViewMatch* vm, uint64_t now) {
    WatchHeader* h = &vm->header;
    if (!watch_header_unpack(vm->hdr.data, vm->hdr.total, h) || h->match_seq != vm->seq) {
        pc_log_line(
            "watch: match %u's header does not read; waiting for it again", (unsigned)vm->seq);
        watch_assembly_reset(&vm->hdr, vm->seq);
        return;
    }
    vm->have_header = true;
    vm->header_ns = now;
    const char* why = NULL;
    if (h->host_mode != GM_VS && h->host_mode != GM_DEBUG_VS) {
        why = "not a VS match";
    } else if (h->scene_kind != GS_VS && h->scene_kind != GS_SUDDEN_DEATH) {
        why = "not a fight scene";
    } else if (!s_env.ignore_rev && strncmp(h->rev, s_my_rev, WATCH_REV_LEN) != 0) {
        why = "another build";
    } else if (strcmp(h->disc, s_my_disc) != 0) {
        why = "another disc image";
    } else {
        why = info_unplayable(h->info);
    }
    if (why != NULL) {
        vm->refused = true;
        pc_log_line("watch: match %u not played: %s (host rev %s disc %s mode %u scene %u; ours "
                    "rev %s disc %s)",
            (unsigned)vm->seq, why, h->rev, h->disc, (unsigned)h->host_mode,
            (unsigned)h->scene_kind, s_my_rev, s_my_disc);
    }
}

/* One datagram from the host, or one message of a dump (from == NULL: no
 * address or cookie to check). */
static void viewer_take(
    const uint8_t* buf, size_t n, const struct sockaddr_in* from, uint64_t now) {
    const bool from_file = from == NULL;
    const int type = watch_msg_type(buf, n);
    watch_lock();
    if (!from_file && !same_addr(from, &s_host_addr)) {
        watch_unlock();
        return;
    }
    switch (type) {
    case WATCH_MSG_WELCOME: {
        WatchWelcome w;
        if (from_file || !watch_unpack_welcome(buf, n, &w) || w.nonce != s_nonce) {
            break;
        }
        s_heard_ns = now;
        snprintf(s_host_name, sizeof s_host_name, "%s", w.name);
        s_host_status.state = w.state;
        s_host_status.mode = w.mode;
        s_have_status = true;
        if (!w.ok) {
            if (atomic_load(&s_link) != LINK_REFUSED || s_refused != w.reason) {
                pc_log_line("watch: %s (%s) refused us: %s", s_host_name, s_host_text,
                    w.reason == WATCH_REFUSE_REV  ? "another build" :
                    w.reason == WATCH_REFUSE_DISC ? "another disc image" :
                    w.reason == WATCH_REFUSE_FULL ? "it already has a viewer" :
                                                    "another watch protocol");
            }
            s_refused = w.reason;
            atomic_store(&s_link, LINK_REFUSED);
        } else if (atomic_load(&s_link) != LINK_WATCHING || w.cookie != s_cookie) {
            if (w.cookie != s_cookie) {
                /* A new subscription: the host may have restarted and count
                 * from 1 again, and it serves no match already running. The
                 * one playing, or taken by the lobby, plays out what it has
                 * (pc_watch_lost ends it there); the rest are forgotten. */
                for (int i = 0; i < VIEW_SLOTS; i++) {
                    if (i == s_play || i == s_taken) {
                        s_vm[i].orphaned = true;
                    } else {
                        vm_free(&s_vm[i]);
                    }
                }
            }
            s_cookie = w.cookie;
            s_ack_ns = 0;
            atomic_store(&s_link, LINK_WATCHING);
            pc_log_line("watch: watching %s (%s)", s_host_name, s_host_text);
        }
        break;
    }
    case WATCH_MSG_STATUS: {
        WatchStatus st;
        if (!from_file && watch_unpack_status(buf, n, &st) && st.cookie == s_cookie) {
            s_heard_ns = now;
            s_host_status = st;
            s_status_ns = now;
            s_have_status = true;
        }
        break;
    }
    case WATCH_MSG_HEADER: {
        WatchChunk c;
        if (!watch_unpack_chunk(buf, n, &c) || (!from_file && c.cookie != s_cookie)) {
            break;
        }
        s_heard_ns = now;
        ViewMatch* vm = vm_for(c.match_seq, true);
        if (vm == NULL) {
            break;
        }
        vm->dirty = true;
        if (!vm->have_header && watch_assembly_add(&vm->hdr, &c) && watch_assembly_done(&vm->hdr)) {
            vm_header_done(vm, now);
        }
        break;
    }
    case WATCH_MSG_FRAMES: {
        WatchFrames m;
        if (!watch_unpack_frames(buf, n, &m) || (!from_file && m.cookie != s_cookie)) {
            break;
        }
        s_heard_ns = now;
        ViewMatch* vm = vm_for(m.match_seq, true);
        if (vm == NULL) {
            break;
        }
        vm->dirty = true;
        vm->packets++;
        if (m.first > vm->contiguous + 1) {
            vm->gaps++; /* something before this one went missing */
        }
        for (int i = 0; i < m.count; i++) {
            vm_store(vm, m.first + i, m.records + (size_t)i * WATCH_FRAME_SIZE);
        }
        break;
    }
    case WATCH_MSG_MATCH_END: {
        WatchMatchEnd e;
        if (!watch_unpack_match_end(buf, n, &e) || (!from_file && e.cookie != s_cookie)) {
            break;
        }
        s_heard_ns = now;
        ViewMatch* vm = vm_for(e.match_seq, true);
        if (vm == NULL) {
            break;
        }
        vm->dirty = true;
        if (!vm->end_known && e.last >= vm->contiguous) {
            vm->end_known = true;
            vm->last = e.last;
            vm->end_reason = e.reason;
        }
        break;
    }
    case WATCH_MSG_BYE: {
        uint32_t cookie;
        if (!from_file && watch_unpack_bye(buf, n, &cookie) && cookie == s_cookie &&
            atomic_load(&s_link) == LINK_WATCHING)
        {
            pc_log_line("watch: %s (%s) stopped sharing", s_host_name, s_host_text);
            atomic_store(&s_link, LINK_LOST);
            s_nonce = new_token(s_my_id);
            s_hello_ns = 0;
        }
        break;
    }
    default:
        break;
    }
    watch_unlock();
}

/* Under s_lock: an ACK for match vm into the queue. */
static void queue_ack(ViewMatch* vm) {
    uint8_t* out = txq_slot();
    if (out == NULL) {
        return;
    }
    int32_t buffered = vm->contiguous + 1;
    if (s_play >= 0 && vm == &s_vm[s_play]) {
        buffered -= atomic_load(&s_next_pub);
    }
    if (buffered < 0) {
        buffered = 0;
    } else if (buffered > 65535) {
        buffered = 65535;
    }
    const uint8_t flags =
        (uint8_t)((vm->have_header ? WATCH_ACK_HEADER : 0) | (vm->end_known ? WATCH_ACK_END : 0));
    WatchAck a = {s_cookie, vm->seq, flags, vm->contiguous, (uint16_t)buffered};
    txq_push(&s_host_addr, watch_pack_ack(out, WATCH_DATAGRAM_MAX, &a));
    vm->dirty = false;
}

static void viewer_turn(uint64_t now) {
    watch_lock();
    const int link = atomic_load(&s_link);
    if (link == LINK_CONNECTING || link == LINK_LOST || link == LINK_REFUSED) {
        const uint64_t every = link == LINK_REFUSED ? REFUSED_HELLO_EVERY_NS : HELLO_EVERY_NS;
        uint8_t* out = txq_slot();
        if (out != NULL && (s_hello_ns == 0 || now - s_hello_ns >= every)) {
            WatchHello h;
            memset(&h, 0, sizeof h);
            h.viewer_id = s_my_id;
            h.nonce = s_nonce;
            snprintf(h.rev, sizeof h.rev, "%s", s_my_rev);
            snprintf(h.disc, sizeof h.disc, "%s", s_my_disc);
            snprintf(h.name, sizeof h.name, "%s", s_my_name);
            txq_push(&s_host_addr, watch_pack_hello(out, WATCH_DATAGRAM_MAX, &h));
            s_hello_ns = now;
        }
    } else if (link == LINK_WATCHING) {
        if (now - s_heard_ns > HOST_LOST_NS) {
            pc_log_line("watch: lost %s (%s): silent for 5 s", s_host_name, s_host_text);
            atomic_store(&s_link, LINK_LOST);
            s_nonce = new_token(s_my_id);
            s_hello_ns = 0;
        } else {
            bool acked = false;
            if (now - s_ack_ns >= ACK_MIN_NS) {
                for (int i = 0; i < VIEW_SLOTS; i++) {
                    if (s_vm[i].used && !s_vm[i].orphaned && s_vm[i].dirty) {
                        queue_ack(&s_vm[i]);
                        acked = true;
                    }
                }
            }
            if (!acked && now - s_ack_ns >= ACK_IDLE_NS) {
                /* The keepalive: about the newest match, or about none. */
                ViewMatch* newest = NULL;
                for (int i = 0; i < VIEW_SLOTS; i++) {
                    if (s_vm[i].used && !s_vm[i].orphaned &&
                        (newest == NULL || s_vm[i].seq > newest->seq))
                    {
                        newest = &s_vm[i];
                    }
                }
                if (newest != NULL) {
                    queue_ack(newest);
                } else {
                    uint8_t* out = txq_slot();
                    if (out != NULL) {
                        WatchAck a = {s_cookie, 0, 0, -1, 0};
                        txq_push(&s_host_addr, watch_pack_ack(out, WATCH_DATAGRAM_MAX, &a));
                    }
                }
                acked = true;
            }
            if (acked) {
                s_ack_ns = now;
            }
        }
    }
    watch_unlock();
    txq_flush();
}

/* ---- the thread ---------------------------------------------------------- */

static int SDLCALL watch_main(void* ud) {
    (void)ud;
    uint8_t buf[2048];
    while (atomic_load(&s_run)) {
        wait_readable(s_sock, TURN_MS);
        uint64_t now = now_ns();
        for (int budget = 64; budget > 0; budget--) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof from;
            memset(&from, 0, sizeof from);
            const int n = (int)recvfrom(
                s_sock, (char*)buf, (int)sizeof buf, 0, (struct sockaddr*)&from, &from_len);
            if (n <= 0) {
                break; /* drained, or an error the next turn retries */
            }
            if (from.sin_family != AF_INET) {
                continue;
            }
            if (s_role == ROLE_HOST) {
                host_on_datagram(buf, (size_t)n, &from, now);
            } else {
                viewer_take(buf, (size_t)n, &from, now);
            }
        }
        now = now_ns();
        if (s_role == ROLE_HOST) {
            host_turn(now);
        } else {
            viewer_turn(now);
        }
    }
    return 0;
}

static bool watch_thread_start(void) {
    atomic_store(&s_run, true);
    s_thread = SDL_CreateThread(watch_main, "watch", NULL);
    if (s_thread == NULL) {
        atomic_store(&s_run, false);
        pc_log_line("watch: cannot start the watch thread");
        return false;
    }
    return true;
}

static void watch_thread_stop(void) {
    if (s_thread != NULL) {
        atomic_store(&s_run, false);
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
    }
}

static void socket_close(void) {
    if (s_sock != WATCH_SOCK_NONE) {
        watch_sock_close(s_sock);
        s_sock = WATCH_SOCK_NONE;
    }
}

/* What every role needs before its thread starts: Winsock, the lock, and
 * this build, disc and device as the game thread reads them. */
static void role_begin(void) {
    net_startup();
    if (s_lock == NULL) {
        s_lock = SDL_CreateMutex();
    }
    static bool at_exit;
    if (!at_exit) {
        at_exit = true;
        atexit(pc_watch_shutdown);
    }
    snprintf(s_my_rev, sizeof s_my_rev, "%s", pc_app_rev());
    snprintf(s_my_disc, sizeof s_my_disc, "%s", pc_lan_disc_id());
    snprintf(s_my_name, sizeof s_my_name, "%s", pc_lan_local_name());
}

/* ---- host role ----------------------------------------------------------- */

static bool host_wanted(void) {
    return !s_env.viewer_process && (s_env.share >= 0 ? s_env.share != 0 : s_share);
}

/* The goodbye a host that stops sharing says to its viewers, so they need
 * not wait out the silence. */
static void host_bye_all(void) {
    if (s_role != ROLE_HOST || s_sock == WATCH_SOCK_NONE) {
        return;
    }
    Viewer viewers[MAX_VIEWERS];
    watch_lock();
    memcpy(viewers, s_viewers, sizeof viewers);
    watch_unlock();
    for (int i = 0; i < MAX_VIEWERS; i++) {
        if (viewers[i].used && viewers[i].verified) {
            uint8_t out[WATCH_BYE_SIZE];
            const size_t n = watch_pack_bye(out, sizeof out, viewers[i].cookie);
            for (int k = 0; k < 3; k++) {
                send_to(&viewers[i].addr, out, n);
            }
        }
    }
}

static void host_start(void) {
    role_begin();
    s_role = ROLE_HOST;
    s_sock = open_socket(s_env.port);
    if (s_sock == WATCH_SOCK_NONE) {
        s_host_failed = true;
        pc_log_line("watch: cannot open udp/%u (is another copy sharing on this device?): no "
                    "viewer can connect%s",
            (unsigned)s_env.port, s_env.dump != NULL ? "; dumping only" : "");
        return;
    }
    if (!watch_thread_start()) {
        socket_close();
        return;
    }
    char ip[INET_ADDRSTRLEN];
    local_ipv4(ip, sizeof ip);
    pc_log_line("watch: sharing on: udp/%u, this device is %s (%s), rev %s disc %s",
        (unsigned)s_env.port, ip, s_my_name, s_my_rev, s_my_disc);
}

static void host_stop(const char* why) {
    host_bye_all();
    watch_thread_stop();
    socket_close();
    watch_lock();
    for (int k = 0; k < 2; k++) {
        log_free(&s_logs[k]);
    }
    memset(s_viewers, 0, sizeof s_viewers);
    watch_unlock();
    s_role = ROLE_NONE;
    pc_log_line("watch: sharing off: %s", why);
}

static void dump_write(const uint8_t* msg, size_t n) {
    if (s_dump == NULL || n == 0) {
        return;
    }
    const uint8_t len[2] = {(uint8_t)(n >> 8), (uint8_t)n};
    if (fwrite(len, 1, sizeof len, s_dump) != sizeof len || fwrite(msg, 1, n, s_dump) != n) {
        pc_log_line("watch: writing %s failed; the dump stops here", s_env.dump);
        fclose(s_dump);
        s_dump = NULL;
        s_dump_failed = true;
    }
}

static void dump_open(void) {
    if (s_dump != NULL || s_dump_failed || s_env.dump == NULL) {
        return;
    }
    s_dump = fopen(s_env.dump, "wb");
    if (s_dump == NULL ||
        fwrite(k_dump_magic, 1, sizeof k_dump_magic, s_dump) != sizeof k_dump_magic)
    {
        pc_log_line("watch: cannot write %s", s_env.dump);
        if (s_dump != NULL) {
            fclose(s_dump);
            s_dump = NULL;
        }
        s_dump_failed = true;
        return;
    }
    pc_log_line("watch: writing shared matches to %s", s_env.dump);
}

/* A match that sets one of these runs code a viewer's match would not
 * (the 1P and Special modes set them; VS mode sets none). */
static bool rules_have_callbacks(const StartMeleeRules* r) {
    return r->on_unpause_override != NULL || r->on_pause_override != NULL ||
           r->check_for_pauser_override != NULL || r->on_match_start != NULL ||
           r->on_frame_start != NULL || r->on_frame_end != NULL || r->on_match_end != NULL ||
           r->x54 != NULL || r->x58 != NULL;
}

/* Everything the match starts from, read before any of it is drawn. */
static void header_capture(WatchHeader* h, const StartMeleeData* start, int mode, int scene) {
    memset(h, 0, sizeof *h);
    h->version = WATCH_HEADER_VERSION;
    h->match_seq = s_seq;
    h->host_mode = (uint8_t)mode;
    h->scene_kind = (uint8_t)scene;
    h->seed = *HSD_RandSeedPtr;
    pc_slp_info_from_start(h->info, start, false);
    for (int i = 0; i < 4; i++) {
        pad_status_to_wire(&h->pad_master[i], &HSD_PadMasterStatus[i]);
        pad_status_to_wire(&h->pad_copy[i], &HSD_PadCopyStatus[i]);
        pad_status_to_wire(&h->pad_game[i], &HSD_PadGameStatus[i]);
        h->raw_master[i].x = pc_pad_master_raw[i].x;
        h->raw_master[i].y = pc_pad_master_raw[i].y;
        h->raw_master[i].cx = pc_pad_master_raw[i].cx;
        h->raw_master[i].cy = pc_pad_master_raw[i].cy;
        h->raw_game[i].x = pc_pad_game_raw[i].x;
        h->raw_game[i].y = pc_pad_game_raw[i].y;
        h->raw_game[i].cx = pc_pad_game_raw[i].cx;
        h->raw_game[i].cy = pc_pad_game_raw[i].cy;
    }
    h->unlock_state = pc_unlock_state_get();
    h->unlock_all = pc_is_unlock_all_enabled();
    h->frozen_stadium = pc_is_frozen_stadium_enabled();
    const GameRules* rules = gmMainLib_GetGameRules();
    memcpy(h->rules, rules, WATCH_RULES_BYTES);
    h->rules_x14 = rules->unk_14;
    const struct GamePrefs* prefs = gmMainLib_GetGamePrefs();
    h->item_freq = prefs->item_freq;
    h->item_mask = prefs->item_mask;
    h->stage_mask = prefs->stage_mask;
    h->pal = pc_region_pal;
    h->language = (uint8_t)lbLang_GetSavedLanguage();
    snprintf(h->rev, sizeof h->rev, "%s", s_my_rev);
    snprintf(h->disc, sizeof h->disc, "%s", s_my_disc);
}

static void host_match_end(void);

static void host_match_start(StartMeleeData* start) {
    const int mode = gm_GetCurrentGameMode();
    const int scene = watch_scene();
    if (pc_net_active()) {
        host_set_state(WATCH_STATE_NETPLAY, mode);
        return;
    }
    if ((mode != GM_VS && mode != GM_DEBUG_VS) || (scene != GS_VS && scene != GS_SUDDEN_DEATH)) {
        host_set_state(WATCH_STATE_UNWATCHABLE, mode);
        return;
    }
    if (rules_have_callbacks(&start->rules)) {
        pc_log_line("watch: this match sets match callbacks; not watchable");
        host_set_state(WATCH_STATE_UNWATCHABLE, mode);
        return;
    }
    if (pc_slp_play_active()) {
        /* A Slippi replay reseeds mid-tick (gmvs.c slippiSyncRng), which a
         * viewer, playing no replay, would not. */
        pc_log_line("watch: a Slippi replay plays here; not watchable");
        host_set_state(WATCH_STATE_UNWATCHABLE, mode);
        return;
    }
    s_seq++;
    int viewers = count_viewers();
    if (viewers == 0 && s_env.test_wait && s_thread != NULL) {
        pc_log_line("watch: match %u waits up to %d s for a viewer (MELEE_WATCH_TEST_WAIT)",
            (unsigned)s_seq, TEST_WAIT_MS / 1000);
        for (int ms = 0; ms < TEST_WAIT_MS && viewers == 0; ms += 10) {
            SDL_Delay(10);
            viewers = count_viewers();
        }
    }
    watch_lock();
    s_live_seq = s_seq;
    watch_unlock();
    host_set_state(WATCH_STATE_LIVE, mode);
    if (viewers == 0 && s_env.dump == NULL) {
        pc_log_line("watch: match %u not shared: no viewer", (unsigned)s_seq);
        return;
    }
    WatchHeader h;
    header_capture(&h, start, mode, scene);
    uint8_t* frames = malloc((size_t)LOG_FRAMES0 * WATCH_FRAME_SIZE);
    if (frames == NULL) {
        pc_log_line("watch: no memory for match %u's log; not shared", (unsigned)s_seq);
        return;
    }
    /* From here the run must reproduce on the viewer: the pinned rules hold
     * (pc_net_deterministic) and the frame counter is in the header. */
    atomic_store(&s_tx_live, true);
    s_tx_scene = scene;
    s_tx_first = pc_net_watch_match_begin(-1, false);
    s_tx_gap_logged = false;
    h.first_frame = s_tx_first;
    h.ucf = pc_is_ucf_enabled();
    h.slippi_fixes = pc_is_slippi_fixes_enabled();
    watch_lock();
    const int next = 1 - s_cur;
    log_free(&s_logs[next]); /* the match before the previous one */
    s_cur = next;
    MatchLog* l = &s_logs[s_cur];
    l->used = true;
    l->seq = s_seq;
    const size_t packed = watch_header_pack(l->header, sizeof l->header, &h);
    l->frames = frames;
    l->cap = LOG_FRAMES0;
    l->last = -1;
    for (int i = 0; i < MAX_VIEWERS; i++) {
        l->vw[i].attached = s_viewers[i].used && s_viewers[i].verified;
        l->vw[i].contiguous = -1;
        l->vw[i].sent_upto = -1;
    }
    uint8_t header[WATCH_HEADER_SIZE];
    memcpy(header, l->header, sizeof header);
    watch_unlock();
    if (packed != WATCH_HEADER_SIZE) {
        pc_log_line("watch: match %u's header does not pack; not shared", (unsigned)s_seq);
        host_match_end();
        return;
    }
    dump_open();
    for (int i = 0; s_dump != NULL && i < watch_chunk_count(WATCH_HEADER_SIZE); i++) {
        uint8_t out[WATCH_DATAGRAM_MAX];
        dump_write(out, watch_pack_chunk(out, sizeof out, 0, s_seq, header, sizeof header, i));
    }
    stadium_prefetch(start->rules.stkind);
    s_report_ns = now_ns();
    s_report_bytes = 0;
    pc_log_line("watch: sharing match %u with %d viewer%s%s", (unsigned)s_seq, viewers,
        viewers == 1 ? "" : "s", s_dump != NULL ? " and the dump" : "");
    log_match("host", &h);
}

static void host_match_end(void) {
    atomic_store(&s_tx_live, false);
    watch_lock();
    MatchLog* l = &s_logs[s_cur];
    l->ended = true;
    l->last = l->count - 1;
    l->end_reason = WATCH_END_SCENE;
    l->ended_ns = now_ns();
    const int32_t last = l->last;
    watch_unlock();
    host_set_state(WATCH_STATE_MENUS, gm_GetCurrentGameMode());
    if (s_dump != NULL) {
        uint8_t out[WATCH_MATCH_END_SIZE];
        WatchMatchEnd e = {0, s_seq, last, WATCH_END_SCENE};
        dump_write(out, watch_pack_match_end(out, sizeof out, &e));
        if (s_dump != NULL) {
            fflush(s_dump);
        }
    }
    pc_log_line("watch: match %u end: last frame %d", (unsigned)s_seq, (int)last);
}

/* ---- viewer role --------------------------------------------------------- */

static bool file_open(const char* path) {
    char magic[sizeof k_dump_magic];
    s_file = fopen(path, "rb");
    if (s_file == NULL || fread(magic, 1, sizeof magic, s_file) != sizeof magic ||
        memcmp(magic, k_dump_magic, sizeof magic) != 0)
    {
        pc_log_line("watch: %s is not a watch dump", path);
        if (s_file != NULL) {
            fclose(s_file);
            s_file = NULL;
        }
        return false;
    }
    pc_log_line("watch: playing the matches in %s", path);
    return true;
}

/* Reads the dump on until one more match has ended there (or the file has). */
static void file_fill(void) {
    if (s_file == NULL) {
        return;
    }
    uint8_t buf[WATCH_DATAGRAM_MAX];
    for (;;) {
        uint8_t len[2];
        if (fread(len, 1, sizeof len, s_file) != sizeof len) {
            break;
        }
        const size_t n = (size_t)len[0] << 8 | len[1];
        if (n == 0 || n > sizeof buf || fread(buf, 1, n, s_file) != n) {
            pc_log_line("watch: the dump is cut short");
            break;
        }
        viewer_take(buf, n, NULL, now_ns());
        if (watch_msg_type(buf, n) == WATCH_MSG_MATCH_END) {
            return;
        }
    }
    fclose(s_file);
    s_file = NULL;
    /* A match the dump cut off part-way ends where its frames do. */
    watch_lock();
    for (int i = 0; i < VIEW_SLOTS; i++) {
        ViewMatch* vm = &s_vm[i];
        if (vm->used && vm->have_header && !vm->end_known) {
            vm->end_known = true;
            vm->last = vm->contiguous;
            vm->end_reason = WATCH_END_ABORTED;
            pc_log_line("watch: match %u has no end in the dump; it ends at frame %d",
                (unsigned)vm->seq, (int)vm->last);
        }
    }
    watch_unlock();
}

void pc_watch_client_start(void) {
    env_read();
    if (s_role == ROLE_VIEWER) {
        return;
    }
    if (s_role == ROLE_HOST) {
        host_stop("this device watches now");
    }
    role_begin();
    watch_lock();
    for (int i = 0; i < VIEW_SLOTS; i++) {
        vm_free(&s_vm[i]);
    }
    s_play = s_taken = -1;
    s_cookie = 0;
    s_host_name[0] = '\0';
    s_host_text[0] = '\0';
    s_have_status = false;
    watch_unlock();
    atomic_store(&s_link, LINK_OFF);
    s_role = ROLE_VIEWER;
    s_after_match = s_last_diverged = false;
    s_file_done = false;
    s_matches_done = 0;
    if (s_env.file != NULL) {
        s_file_mode = true;
        snprintf(s_host_name, sizeof s_host_name, "Recording");
        s_file_opened = file_open(s_env.file);
        if (s_file_opened) {
            atomic_store(&s_link, LINK_WATCHING);
        }
        return;
    }
    s_file_mode = false;
    s_sock = open_socket(0);
    if (s_sock == WATCH_SOCK_NONE) {
        pc_log_line("watch: cannot open a UDP socket; nothing to watch with");
        return;
    }
    s_my_id = pc_install_id() ^ (uint64_t)socket_port(s_sock) << 48;
    if (!watch_thread_start()) {
        socket_close();
        return;
    }
    if (s_env.host == NULL) {
        pc_log_line("watch: no host to watch: set MELEE_WATCH_HOST=ip[:port]");
        return;
    }
    char ip[64];
    unsigned port = STREAM_PORT;
    snprintf(ip, sizeof ip, "%s", s_env.host);
    char* colon = strchr(ip, ':');
    if (colon != NULL) {
        *colon = '\0';
        port = (unsigned)atoi(colon + 1);
    }
    if (port == 0 || port > 65535 || !pc_watch_subscribe(ip, (uint16_t)port)) {
        pc_log_line("watch: MELEE_WATCH_HOST must be ip[:port], not %s", s_env.host);
    }
}

bool pc_watch_subscribe(const char* ip, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port != 0 ? port : STREAM_PORT);
    if (s_role != ROLE_VIEWER || s_file_mode || ip == NULL ||
        inet_pton(AF_INET, ip, &a.sin_addr) != 1)
    {
        return false;
    }
    watch_lock();
    s_host_addr = a;
    addr_text(&a, s_host_text, sizeof s_host_text);
    snprintf(s_host_name, sizeof s_host_name, "%s", ip);
    s_cookie = 0;
    s_nonce = new_token(s_my_id);
    s_hello_ns = 0;
    s_have_status = false;
    atomic_store(&s_link, LINK_CONNECTING);
    watch_unlock();
    pc_log_line("watch: subscribing to %s as %s (rev %s disc %s)", s_host_text, s_my_name, s_my_rev,
        s_my_disc);
    return true;
}

void pc_watch_client_stop(void) {
    if (s_role != ROLE_VIEWER) {
        return;
    }
    watch_thread_stop();
    if (s_sock != WATCH_SOCK_NONE && atomic_load(&s_link) == LINK_WATCHING) {
        uint8_t out[WATCH_BYE_SIZE];
        const size_t n = watch_pack_bye(out, sizeof out, s_cookie);
        for (int k = 0; k < 3; k++) {
            send_to(&s_host_addr, out, n);
        }
    }
    socket_close();
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    atomic_store(&s_view_live, false);
    watch_lock();
    for (int i = 0; i < VIEW_SLOTS; i++) {
        vm_free(&s_vm[i]);
    }
    s_play = s_taken = -1;
    watch_unlock();
    pc_watch_unpin_rules();
    atomic_store(&s_link, LINK_OFF);
    s_role = ROLE_NONE;
    s_file_mode = false;
    pc_log_line("watch: stopped watching");
}

bool pc_watch_take_match(PcWatchMatch* m) {
    if (s_role != ROLE_VIEWER || atomic_load(&s_view_live)) {
        return false;
    }
    watch_lock();
    if (s_taken >= 0) {
        /* Taken and never started: the lobby was left another way. */
        s_vm[s_taken].played = true;
        s_taken = -1;
    }
    watch_unlock();
    for (int pass = 0; pass < 2; pass++) {
        int best = -1;
        watch_lock();
        for (int i = 0; i < VIEW_SLOTS; i++) {
            const ViewMatch* vm = &s_vm[i];
            /* A dump's match is read to its end before it is played. */
            if (vm->used && vm->have_header && !vm->refused && !vm->taken && !vm->played &&
                (!s_file_mode || vm->end_known) && (best < 0 || vm->seq < s_vm[best].seq))
            {
                best = i;
            }
        }
        if (best >= 0) {
            ViewMatch* vm = &s_vm[best];
            vm->taken = true;
            s_taken = best;
            s_taken_hdr = vm->header;
            memset(m, 0, sizeof *m);
            m->seq = vm->seq;
            m->scene_kind = vm->header.scene_kind;
            m->stkind = (uint16_t)info_stage(vm->header.info);
            memcpy(m->info, vm->header.info, sizeof m->info);
        }
        watch_unlock();
        if (best >= 0) {
            pc_log_line("watch: taking match %u", (unsigned)m->seq);
            return true;
        }
        if (!s_file_mode || s_file == NULL) {
            break;
        }
        file_fill();
    }
    if (s_file_mode && s_file_opened && s_file == NULL && !s_file_done) {
        s_file_done = true;
        pc_log_line("watch: the dump is played out (%u matches); exiting", s_matches_done);
        pc_exit_requested = true;
    }
    return false;
}

void pc_watch_status(PcWatchStatus* out) {
    memset(out, 0, sizeof *out);
    if (s_role != ROLE_VIEWER) {
        return;
    }
    watch_lock();
    const int link = atomic_load(&s_link);
    const uint8_t refused = s_refused;
    const bool have_status = s_have_status;
    const WatchStatus st = s_host_status;
    /* The host's live match is one we are about to play (not one played,
     * or left, already). */
    bool ours = false;
    for (int i = 0; i < VIEW_SLOTS; i++) {
        ours = ours || (s_vm[i].used && !s_vm[i].orphaned && !s_vm[i].played &&
                           s_vm[i].seq == st.match_seq && s_vm[i].have_header);
    }
    char host[WATCH_NAME_LEN + 1];
    snprintf(host, sizeof host, "%s", s_host_name);
    char at[sizeof s_host_text];
    snprintf(at, sizeof at, "%s", s_host_text);
    watch_unlock();
    snprintf(out->host, sizeof out->host, "%s", host);
    if (s_file_mode) {
        snprintf(out->message, sizeof out->message, "%s",
            !s_file_opened ? "The recording could not be read" :
            s_file_done    ? "The recording is over" :
                             "Playing the recording");
        out->error = !s_file_opened;
        return;
    }
    switch (link) {
    case LINK_OFF:
        snprintf(out->message, sizeof out->message,
            "No host to watch: set MELEE_WATCH_HOST in melee-env.txt");
        out->error = true;
        return;
    case LINK_CONNECTING:
        snprintf(out->message, sizeof out->message, "Connecting to %s...", at);
        return;
    case LINK_REFUSED:
        snprintf(out->message, sizeof out->message, "%s %s", host,
            refused == WATCH_REFUSE_REV  ? "runs another build. Install the same one on both." :
            refused == WATCH_REFUSE_DISC ? "uses another disc image." :
            refused == WATCH_REFUSE_FULL ? "already has a viewer." :
                                           "runs an incompatible Xelee Watch.");
        out->error = true;
        return;
    case LINK_LOST:
        snprintf(out->message, sizeof out->message, "Lost %s. Looking for it again...", host);
        out->error = true;
        return;
    default:
        break;
    }
    if (have_status && st.state == WATCH_STATE_LIVE && !ours) {
        snprintf(
            out->message, sizeof out->message, "%s is in a match. You'll join the next one.", host);
    } else if (have_status && st.state == WATCH_STATE_UNWATCHABLE) {
        snprintf(out->message, sizeof out->message, "%s is playing %s. Watch shows VS matches.",
            host, mode_name(st.mode));
    } else if (have_status && st.state == WATCH_STATE_NETPLAY) {
        snprintf(out->message, sizeof out->message, "%s is playing online. Watch shows VS matches.",
            host);
    } else if (s_after_match && s_last_diverged) {
        snprintf(out->message, sizeof out->message,
            "GAME! That match went out of sync here. Waiting for the next match");
    } else if (s_after_match) {
        snprintf(out->message, sizeof out->message, "GAME! Waiting for the next match");
    } else {
        snprintf(out->message, sizeof out->message, "Waiting for the next match");
    }
}

/* ---- rules pin ------------------------------------------------------------ */

static bool s_pin_saved;
static GameRules s_saved_rules;
static uint8_t s_saved_item_freq;
static uint64_t s_saved_item_mask;
static uint32_t s_saved_stage_mask;
static uint64_t s_saved_unlock;
static bool s_pin_unlock_all, s_pin_frozen;

void pc_watch_pin_rules(const PcWatchMatch* m) {
    if (s_role != ROLE_VIEWER || s_taken < 0 || m == NULL || s_taken_hdr.match_seq != m->seq) {
        return;
    }
    const WatchHeader* h = &s_taken_hdr;
    GameRules* rules = gmMainLib_GetGameRules();
    struct GamePrefs* prefs = gmMainLib_GetGamePrefs();
    if (!s_pin_saved) {
        s_saved_rules = *rules;
        s_saved_item_freq = prefs->item_freq;
        s_saved_item_mask = prefs->item_mask;
        s_saved_stage_mask = prefs->stage_mask;
        s_saved_unlock = pc_unlock_state_get();
        s_pin_saved = true;
    }
    memcpy(rules, h->rules, WATCH_RULES_BYTES);
    rules->unk_14 = h->rules_x14;
    prefs->item_freq = h->item_freq;
    prefs->item_mask = h->item_mask;
    prefs->stage_mask = h->stage_mask;
    pc_unlock_state_set(h->unlock_state);
    s_pin_unlock_all = h->unlock_all != 0;
    s_pin_frozen = h->frozen_stadium != 0;
    atomic_store(&s_pinned, true);
    pc_log_line("watch: the host's rules for match %u: mode %u time %u stock %u items %u/%016llx "
                "stages %08x frozen %u unlock %016llx",
        (unsigned)h->match_seq, (unsigned)h->rules[2], (unsigned)h->rules[3], (unsigned)h->rules[4],
        (unsigned)h->item_freq, (unsigned long long)h->item_mask, (unsigned)h->stage_mask,
        (unsigned)h->frozen_stadium, (unsigned long long)h->unlock_state);
}

void pc_watch_unpin_rules(void) {
    if (!s_pin_saved) {
        atomic_store(&s_pinned, false);
        return;
    }
    struct GamePrefs* prefs = gmMainLib_GetGamePrefs();
    *gmMainLib_GetGameRules() = s_saved_rules;
    prefs->item_freq = s_saved_item_freq;
    prefs->item_mask = s_saved_item_mask;
    prefs->stage_mask = s_saved_stage_mask;
    pc_unlock_state_set(s_saved_unlock);
    s_pin_saved = false;
    atomic_store(&s_pinned, false);
    pc_log_line("watch: own rules and unlocks back");
}

bool pc_watch_rules(bool* unlock_all, bool* frozen_stadium) {
    if (!atomic_load(&s_pinned)) {
        return false;
    }
    *unlock_all = s_pin_unlock_all;
    *frozen_stadium = s_pin_frozen;
    return true;
}

/* ---- the watched match ---------------------------------------------------- */

static void viewer_match_start(StartMeleeData* start) {
    const WatchHeader* h = &s_taken_hdr;
    if (watch_scene() != h->scene_kind) {
        pc_log_line("watch: match %u is for scene %u, this is scene %d; not played",
            (unsigned)h->match_seq, (unsigned)h->scene_kind, watch_scene());
        watch_lock();
        s_vm[s_taken].played = true;
        s_taken = -1;
        watch_unlock();
        return;
    }
    /* The host's start data over ours; the pointers stay ours (NULL here). */
    pc_slp_start_from_info(start, h->info);
    *HSD_RandSeedPtr = h->seed;
    for (int i = 0; i < 4; i++) {
        pad_status_from_wire(&HSD_PadMasterStatus[i], &h->pad_master[i]);
        pad_status_from_wire(&HSD_PadCopyStatus[i], &h->pad_copy[i]);
        pad_status_from_wire(&HSD_PadGameStatus[i], &h->pad_game[i]);
        pc_pad_master_raw[i].x = h->raw_master[i].x;
        pc_pad_master_raw[i].y = h->raw_master[i].y;
        pc_pad_master_raw[i].cx = h->raw_master[i].cx;
        pc_pad_master_raw[i].cy = h->raw_master[i].cy;
        pc_pad_game_raw[i].x = h->raw_game[i].x;
        pc_pad_game_raw[i].y = h->raw_game[i].y;
        pc_pad_game_raw[i].cx = h->raw_game[i].cx;
        pc_pad_game_raw[i].cy = h->raw_game[i].cy;
    }
    s_next = 0;
    atomic_store(&s_next_pub, 0);
    s_started = false;
    s_have_cur = false;
    s_mismatches = 0;
    s_end_asked = false;
    s_end_asked_at = -1;
    s_end_wait = 0;
    s_leave_passes = 0;
    s_leaving = false;
    s_starved = 0;
    s_buf_n = 0;
    s_buf_sum = 0;
    s_after_match = s_last_diverged = false;
    watch_lock();
    s_play = s_taken;
    s_taken = -1;
    watch_unlock();
    pc_net_watch_match_begin(h->first_frame, true);
    atomic_store(&s_view_live, true);
    stadium_prefetch(start->rules.stkind);
    log_match("watching", h);
    pc_log_line("watch: host rev %s, ours %s; ucf %u fixes %u here", h->rev, s_my_rev,
        (unsigned)pc_is_ucf_enabled(), (unsigned)pc_is_slippi_fixes_enabled());
}

void pc_watch_match_start(struct StartMeleeData* start) {
    if (start == NULL) {
        return;
    }
    if (s_role == ROLE_VIEWER) {
        if (s_taken >= 0 && gm_GetCurrentGameMode() == GM_ONLINE) {
            viewer_match_start(start);
        }
        return;
    }
    if (s_role == ROLE_HOST) {
        host_match_start(start);
    }
}

static void viewer_match_end(void) {
    atomic_store(&s_view_live, false);
    bool end_known = false;
    int32_t last = -1;
    uint32_t seq = 0;
    watch_lock();
    if (s_play >= 0) {
        ViewMatch* vm = &s_vm[s_play];
        end_known = vm->end_known;
        last = vm->last;
        seq = vm->seq;
        vm->played = true;
        s_play = -1;
    }
    watch_unlock();
    char how[96];
    if (s_leaving) {
        snprintf(how, sizeof how, "left at frame %d", (int)s_next);
    } else if (!end_known) {
        snprintf(how, sizeof how, "the host's end unknown");
    } else if (!s_end_asked) {
        snprintf(
            how, sizeof how, "our scene never asked to end (the host's ended at %d)", (int)last);
    } else if (s_end_asked_at == last) {
        snprintf(how, sizeof how, "the scene ended where the host's did (%d)", (int)last);
    } else {
        snprintf(how, sizeof how, "our scene asked to end at %d, the host's at %d",
            (int)s_end_asked_at, (int)last);
    }
    pc_log_line("watch: match %u done: %d frames, %u mismatches; %s", (unsigned)seq, (int)s_next,
        s_mismatches, how);
    s_matches_done++;
    /* Left with B, no GAME! happened here: the waiting screen tells what
     * the host is doing instead. */
    s_after_match = !s_leaving;
    s_last_diverged = !s_leaving && s_mismatches > 0;
}

void pc_watch_match_end(void) {
    if (atomic_load(&s_tx_live)) {
        host_match_end();
    } else if (s_role == ROLE_HOST) {
        host_set_state(WATCH_STATE_MENUS, gm_GetCurrentGameMode());
    }
    if (atomic_load(&s_view_live)) {
        viewer_match_end();
    }
}

bool pc_watch_live(void) {
    return atomic_load(&s_tx_live) || atomic_load(&s_view_live);
}

bool pc_watch_viewing(void) {
    return atomic_load(&s_view_live);
}

void pc_watch_tx_frame(const struct PADStatus* head, uint32_t ck, int32_t net_frame) {
    if (!atomic_load(&s_tx_live) || watch_scene() != s_tx_scene) {
        return;
    }
    WatchFrame f;
    memset(&f, 0, sizeof f);
    for (int i = 0; i < 4; i++) {
        pad_to_wire(&f.pads[i], &head[i]);
    }
    f.ck = ck;
    f.seed = *HSD_RandSeedPtr; /* the seed the tick runs from, as record_frame reads it */
    f.scene = (uint8_t)watch_scene();
    uint8_t rec[WATCH_FRAME_SIZE];
    watch_frame_pack(rec, &f);
    int32_t index = -1, unacked = 0;
    bool full = false;
    watch_lock();
    MatchLog* l = &s_logs[s_cur];
    if (l->used && !l->ended) {
        if (l->count == l->cap) {
            uint8_t* frames = realloc(l->frames, (size_t)l->cap * 2 * WATCH_FRAME_SIZE);
            if (frames != NULL) {
                l->frames = frames;
                l->cap *= 2;
            }
        }
        if (l->count < l->cap) {
            memcpy(l->frames + (size_t)l->count * WATCH_FRAME_SIZE, rec, WATCH_FRAME_SIZE);
            index = l->count++;
        } else {
            full = true;
        }
        for (int i = 0; i < MAX_VIEWERS; i++) {
            if (l->vw[i].attached && l->count - 1 - l->vw[i].contiguous > unacked) {
                unacked = l->count - 1 - l->vw[i].contiguous;
            }
        }
    }
    const uint64_t bytes = s_tx_bytes;
    const unsigned resends = s_tx_resends;
    watch_unlock();
    if (full) {
        pc_log_line(
            "watch: no memory for more of match %u; it is not shared past here", (unsigned)s_seq);
        host_match_end();
        return;
    }
    if (index < 0) {
        return;
    }
    if (index != net_frame - s_tx_first && !s_tx_gap_logged) {
        s_tx_gap_logged = true;
        pc_log_line("watch: tick %d recorded as match frame %d (expected %d)", (int)net_frame,
            (int)index, (int)(net_frame - s_tx_first));
    }
    if (s_dump != NULL) {
        uint8_t out[WATCH_FRAMES_HEAD + WATCH_FRAME_SIZE];
        WatchFrames m = {0, s_seq, index, 1, rec};
        dump_write(out, watch_pack_frames(out, sizeof out, &m));
        if (s_dump != NULL && index % 60 == 59) {
            fflush(s_dump);
        }
    }
    if (index > 0 && index % REPORT_EVERY == 0) {
        const uint64_t now = now_ns();
        const double secs = (double)(now - s_report_ns) / 1e9;
        pc_log_line("watch: host frame %d: %d viewer(s), unacked max %d, resends %u, %.1f KB/s",
            (int)index, count_viewers(), (int)unacked, resends,
            secs > 0 ? (double)(bytes - s_report_bytes) / 1024.0 / secs : 0.0);
        s_report_ns = now;
        s_report_bytes = bytes;
    }
}

/* The buttons held on any connected port in the newest queued pad sample. */
static uint32_t newest_buttons(void) {
    const PadLibData* p = &HSD_PadLibData;
    if (p->qcount == 0 || p->queue == NULL) {
        return s_last_buttons;
    }
    const int newest = p->qwrite != 0 ? p->qwrite - 1 : p->qnum - 1;
    uint32_t buttons = 0;
    for (int port = 0; port < 4; port++) {
        const PADStatus* s = &p->queue[newest].stat[port];
        if (s->err == 0) {
            buttons |= s->button;
        }
    }
    s_last_buttons = buttons;
    return buttons;
}

/* Under s_lock: the match being played. */
static ViewMatch* playing(void) {
    return s_play >= 0 ? &s_vm[s_play] : NULL;
}

bool pc_watch_hold(void) {
    if (!atomic_load(&s_view_live)) {
        return false;
    }
    /* B held for a second leaves the match for the waiting screen. */
    s_leave_passes = (newest_buttons() & PAD_BUTTON_B) != 0 ? s_leave_passes + 1 : 0;
    if (s_leave_passes >= LEAVE_HOLD_PASSES && !s_leaving) {
        s_leaving = true;
        pc_log_line("watch: leaving the match at frame %d (B held)", (int)s_next);
    }
    int32_t contiguous = -1, last = -1;
    bool end_known = false, cut_off = false;
    watch_lock();
    const ViewMatch* vm = playing();
    if (vm != NULL) {
        contiguous = vm->contiguous;
        end_known = vm->end_known;
        last = vm->last;
        cut_off = vm->orphaned;
    }
    watch_unlock();
    cut_off = cut_off || atomic_load(&s_link) == LINK_LOST;
    int32_t avail = contiguous - s_next + 1;
    if (avail < 0) {
        avail = 0;
    }
    bool hold;
    if (s_leaving) {
        hold = true;
    } else if (!s_started) {
        int32_t need = s_env.buffer;
        if (end_known && last - s_next + 1 < need) {
            need = last - s_next + 1;
        }
        if (cut_off) {
            /* No more is coming: what arrived plays out, then
             * pc_watch_lost() ends the scene. Waiting for a full buffer
             * would hold the scene for good. */
            need = 1;
        }
        hold = avail == 0 || avail < need;
        s_started = !hold;
    } else {
        hold = avail == 0;
        if (hold && !(end_known && s_next > last)) {
            s_starved++;
        }
    }
    if (hold) {
        HSD_PadFlushQueue(HSD_PAD_FLUSH_QUEUE_THROWAWAY);
    }
    return hold;
}

bool pc_watch_end_scene_now(void) {
    if (!atomic_load(&s_view_live)) {
        return false;
    }
    if (s_leaving) {
        return true;
    }
    bool end_known = false;
    int32_t last = -1;
    watch_lock();
    const ViewMatch* vm = playing();
    if (vm != NULL) {
        end_known = vm->end_known;
        last = vm->last;
    }
    watch_unlock();
    if (!end_known || s_next <= last) {
        s_end_wait = 0;
        return false;
    }
    if (s_end_asked) {
        return true;
    }
    if (++s_end_wait >= END_GRACE_PASSES) {
        pc_log_line("watch: the host's match ended at frame %d and ours did not ask to; ending it",
            (int)last);
        return true;
    }
    return false;
}

bool pc_watch_scene_hold(int32_t net_frame) {
    (void)net_frame;
    if (!atomic_load(&s_view_live)) {
        return false;
    }
    if (!s_end_asked) {
        s_end_asked = true;
        s_end_asked_at = s_next - 1;
    }
    if (s_leaving) {
        return false;
    }
    bool end_known = false;
    int32_t last = -1;
    watch_lock();
    const ViewMatch* vm = playing();
    if (vm != NULL) {
        end_known = vm->end_known;
        last = vm->last;
    }
    watch_unlock();
    /* Asking before the host's last frame has run: the host's match went on,
     * so this scene does too (a divergence; the match's end line says so). */
    return !(end_known && s_next > last);
}

bool pc_watch_lost(void) {
    if (s_role != ROLE_VIEWER) {
        return false;
    }
    const bool link_lost = atomic_load(&s_link) == LINK_LOST;
    bool lost;
    watch_lock();
    const ViewMatch* vm = playing();
    if (vm == NULL) {
        lost = link_lost;
    } else {
        /* What arrived before the silence still plays out. */
        lost = (link_lost || vm->orphaned) && vm->contiguous < s_next;
    }
    watch_unlock();
    return lost;
}

static void viewer_feed(PADStatus* head) {
    uint8_t rec[WATCH_FRAME_SIZE];
    bool ok = false;
    int32_t buffered = 0;
    uint64_t header_ns = 0;
    uint32_t seq = 0;
    unsigned packets = 0, gaps = 0;
    watch_lock();
    const ViewMatch* vm = playing();
    if (vm != NULL && s_next <= vm->contiguous) {
        memcpy(rec, vm->frames + (size_t)s_next * WATCH_FRAME_SIZE, WATCH_FRAME_SIZE);
        ok = true;
        buffered = vm->contiguous - s_next;
        header_ns = vm->header_ns;
        seq = vm->seq;
        packets = vm->packets;
        gaps = vm->gaps;
    }
    const WatchStatus st = s_host_status;
    const uint64_t st_ns = s_status_ns;
    watch_unlock();
    if (!ok) {
        static bool logged;
        if (!logged) {
            logged = true;
            pc_log_line(
                "watch: a tick ran at match frame %d with no host frame to feed", (int)s_next);
        }
        return;
    }
    WatchFrame f;
    watch_frame_unpack(rec, &f);
    for (int i = 0; i < 4; i++) {
        pad_from_wire(&head[i], &f.pads[i]);
    }
    *HSD_RandSeedPtr = f.seed;
    s_cur_ck = f.ck;
    s_cur_frame = s_next;
    s_have_cur = true;
    if (s_next == 0) {
        pc_log_line("watch: match %u's first tick, %.0f ms after its header was in", (unsigned)seq,
            (double)(now_ns() - header_ns) / 1e6);
    }
    if (s_buf_n == 0 || buffered < s_buf_min) {
        s_buf_min = buffered;
    }
    if (s_buf_n == 0 || buffered > s_buf_max) {
        s_buf_max = buffered;
    }
    s_buf_sum += buffered;
    s_buf_n++;
    s_next++;
    atomic_store(&s_next_pub, s_next);
    if (s_next % REPORT_EVERY == 0) {
        char lag[24] = "unknown";
        if (st.match_seq == seq && st.frame >= 0) {
            /* STATUS comes once a second: the host has run on since this
             * one, at 60 ticks a second unless it paused or held. */
            const double host = st.frame + (double)(now_ns() - st_ns) * 60.0 / 1e9;
            snprintf(lag, sizeof lag, "%.0f ms", (host - (s_next - 1)) * 1000.0 / 60.0);
        }
        pc_log_line("watch: frame %d: buffered %d/%.1f/%d (min/avg/max), %u starved passes, lag "
                    "%s, loss %u%% (%u of %u datagrams after a gap)",
            (int)(s_next - 1), (int)s_buf_min, (double)s_buf_sum / s_buf_n, (int)s_buf_max,
            s_starved, lag, packets ? gaps * 100 / packets : 0u, gaps, packets);
        s_buf_n = 0;
        s_buf_sum = 0;
        s_starved = 0;
    }
}

void pc_watch_feed(struct PADStatus* head) {
    if (atomic_load(&s_view_live)) {
        viewer_feed(head);
    } else if (atomic_load(&s_tx_live)) {
        for (int i = 0; i < 4; i++) {
            pad_clear_padding(&head[i]);
        }
    }
}

void pc_watch_compare(uint32_t ck) {
    if (!atomic_load(&s_view_live) || !s_have_cur) {
        return;
    }
    s_have_cur = false;
    if (ck != s_cur_ck) {
        if (s_mismatches == 0) {
            pc_log_line("watch: DIVERGED at match frame %d (host %08x here %08x)", (int)s_cur_frame,
                (unsigned)s_cur_ck, (unsigned)ck);
        }
        s_mismatches++;
    }
}

/* ---- the frame boundary --------------------------------------------------- */

void pc_watch_set_share(bool on) {
    s_share = on;
}

/* Closing the app must not leave the thread running into stdio's and SDL's
 * teardown, nor leave the host holding a closed viewer's only slot until
 * its silence runs out (a relaunched viewer has a new port, so a new id). */
void pc_watch_shutdown(void) {
    if (s_role == ROLE_HOST) {
        host_stop("exiting");
    } else if (s_role == ROLE_VIEWER) {
        pc_watch_client_stop();
    }
}

void pc_watch_service(void) {
    env_read();
    if (s_role == ROLE_NONE && host_wanted() && !s_host_failed) {
        /* pc_lan_disc_id() is "0" until the launcher has handed a disc over,
         * and a viewer's rev and disc are checked against ours. */
        if (strcmp(pc_lan_disc_id(), "0") != 0) {
            host_start();
        }
    } else if (s_role == ROLE_HOST && !host_wanted() && !atomic_load(&s_tx_live)) {
        host_stop("turned off");
    }
}
