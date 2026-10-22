/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Xelee Watch's wire format (src/pc/net_watch.c): every datagram the host
 * and its viewer exchange, the match header and the per-tick frame record,
 * packed and unpacked, and the header's chunking. Pure functions over byte
 * buffers -- no sockets, no game state, no allocation -- so
 * tools/test_watch_wire.c can drive them directly.
 *
 * Every datagram starts with 'X' 'W' proto type. Every multi-byte field
 * after that is big-endian and written field by field, never as a struct
 * image, so an x86 PC and an ARM headset read each other's bytes alike.
 * Unpacking demands the exact length of the message (FRAMES and HEADER:
 * the exact length their counts give), so a truncated or padded datagram
 * is refused whole instead of read in part. */
#ifndef PC_NET_WATCH_WIRE_H
#define PC_NET_WATCH_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WATCH_PROTO 1
#define WATCH_PREFIX 4 /* 'X' 'W' proto type */

/* Message types. BEACON is the discovery broadcast of a later milestone. */
enum {
    WATCH_MSG_BEACON = 0x01,
    WATCH_MSG_HELLO = 0x02,
    WATCH_MSG_WELCOME = 0x03,
    WATCH_MSG_STATUS = 0x04,
    WATCH_MSG_HEADER = 0x05,
    WATCH_MSG_FRAMES = 0x06,
    WATCH_MSG_ACK = 0x07,
    WATCH_MSG_MATCH_END = 0x08,
    WATCH_MSG_BYE = 0x09,
};
/* watch_msg_type() for a watch datagram of another protocol version. */
#define WATCH_MSG_OTHER_PROTO (-2)

/* Text fields: fixed width on the wire, NUL padded, not NUL terminated when
 * full. Unpacked into buffers one byte longer, always terminated. */
#define WATCH_REV_LEN 24  /* pc_app_rev() */
#define WATCH_DISC_LEN 8  /* pc_lan_disc_id() */
#define WATCH_NAME_LEN 16 /* pc_lan_local_name() */

#define WATCH_INFO_SIZE 0x138 /* StartMeleeData as Slippi's Game Info Block */
#define WATCH_RULES_BYTES 20  /* GameRules' u8 fields; unk_14 travels apart */
#define WATCH_FRAME_SIZE 53   /* one packed WatchFrame */
#define WATCH_FRAMES_MAX 20   /* frames one FRAMES datagram carries */
#define WATCH_CHUNK_MAX 1000  /* header bytes one HEADER datagram carries */
#define WATCH_HEADER_CHUNKS_MAX 4
#define WATCH_HEADER_VERSION 1
#define WATCH_HEADER_SIZE 1235  /* a packed WatchHeader */
#define WATCH_DATAGRAM_MAX 1200 /* every datagram fits a safe UDP payload */

/* Whole datagram sizes, prefix included. */
#define WATCH_HELLO_SIZE 64
#define WATCH_WELCOME_SIZE 32
#define WATCH_STATUS_SIZE 18
#define WATCH_CHUNK_HEAD 16  /* HEADER, before its data */
#define WATCH_FRAMES_HEAD 17 /* FRAMES, before its records */
#define WATCH_ACK_SIZE 19
#define WATCH_MATCH_END_SIZE 17
#define WATCH_BYE_SIZE 8

/* WELCOME.reason */
enum {
    WATCH_REFUSE_NONE = 0,
    WATCH_REFUSE_REV = 1,   /* another build */
    WATCH_REFUSE_DISC = 2,  /* another game image */
    WATCH_REFUSE_PROTO = 3, /* another watch protocol */
    WATCH_REFUSE_FULL = 4,  /* every viewer slot taken */
};

/* WELCOME.state and STATUS.state: what the host is doing. */
enum {
    WATCH_STATE_MENUS = 0,
    WATCH_STATE_LIVE = 1,        /* a watchable match is running (shared or not) */
    WATCH_STATE_UNWATCHABLE = 2, /* a 1P or Special mode */
    WATCH_STATE_NETPLAY = 3,     /* an online match */
};

/* MATCH_END.reason */
enum {
    WATCH_END_SCENE = 0,   /* the match scene ended */
    WATCH_END_LEAVING = 1, /* the host stopped sharing */
    WATCH_END_ABORTED = 2,
};

/* ACK.flags */
#define WATCH_ACK_HEADER 0x01 /* the whole header is in */
#define WATCH_ACK_END 0x02    /* the MATCH_END is in */

/* PADStatus's eleven bytes, as the tick consumed them. */
typedef struct WatchPad {
    uint16_t button;
    int8_t stick_x, stick_y, substick_x, substick_y;
    uint8_t trigger_l, trigger_r, analog_a, analog_b;
    int8_t err;
} WatchPad;

/* One simulated tick: the four pads, its checksum, the RNG seed it started
 * from and the scene it ran in. 4 x 11 + 4 + 4 + 1 = 53 bytes. */
typedef struct WatchFrame {
    WatchPad pads[4];
    uint32_t ck;
    uint32_t seed;
    uint8_t scene;
} WatchFrame;

/* HSD_PadStatus field by field; the eight nml_ floats as their bits. */
typedef struct WatchPadStatus {
    uint32_t button, last_button, trigger, repeat, release;
    int32_t repeat_count;
    int8_t stick_x, stick_y, substick_x, substick_y;
    uint8_t analog_l, analog_r, analog_a, analog_b;
    uint32_t nml[8]; /* nml_stickX, Y, subStickX, Y, analogL, R, A, B */
    uint8_t cross_dir;
    int8_t err;
} WatchPadStatus;

/* PcRawSticks */
typedef struct WatchSticks {
    int8_t x, y, cx, cy;
} WatchSticks;

/* Everything the viewer must hold, before the match's first tick, to run
 * the host's match from the same start (spectator-spec.md section 2.5). */
typedef struct WatchHeader {
    uint16_t version; /* WATCH_HEADER_VERSION */
    uint32_t match_seq;
    uint8_t host_mode;   /* GameModeKind */
    uint8_t scene_kind;  /* GameSceneKind: GS_VS or GS_SUDDEN_DEATH */
    int32_t first_frame; /* the host's frame number of the match's first tick */
    uint32_t seed;       /* the RNG seed at match setup */
    uint8_t info[WATCH_INFO_SIZE];
    WatchPadStatus pad_master[4], pad_copy[4], pad_game[4];
    WatchSticks raw_master[4], raw_game[4];
    uint64_t unlock_state;
    uint8_t unlock_all;
    uint8_t frozen_stadium;
    uint8_t rules[WATCH_RULES_BYTES]; /* GameRules, its u8 fields in order */
    int32_t rules_x14;                /* GameRules.unk_14 */
    uint8_t item_freq;
    uint64_t item_mask;
    uint32_t stage_mask;
    uint8_t ucf, slippi_fixes, pal, language; /* informational */
    char rev[WATCH_REV_LEN + 1];
    char disc[WATCH_DISC_LEN + 1];
} WatchHeader;

typedef struct WatchHello {
    uint64_t viewer_id;
    uint32_t nonce;
    char rev[WATCH_REV_LEN + 1];
    char disc[WATCH_DISC_LEN + 1];
    char name[WATCH_NAME_LEN + 1];
} WatchHello;

typedef struct WatchWelcome {
    uint32_t nonce; /* the HELLO's, echoed */
    uint32_t cookie;
    uint8_t ok;
    uint8_t reason;                /* WATCH_REFUSE_* */
    uint8_t state;                 /* WATCH_STATE_* */
    uint8_t mode;                  /* GameModeKind */
    char name[WATCH_NAME_LEN + 1]; /* the host's */
} WatchWelcome;

typedef struct WatchStatus {
    uint32_t cookie;
    uint8_t state; /* WATCH_STATE_* */
    uint8_t mode;  /* GameModeKind */
    uint32_t match_seq;
    int32_t frame; /* newest match frame the host has run, -1 none */
} WatchStatus;

/* One HEADER datagram. data points into the datagram it was unpacked from. */
typedef struct WatchChunk {
    uint32_t cookie;
    uint32_t match_seq;
    uint8_t idx, cnt;
    uint16_t total; /* the whole header's bytes */
    uint16_t len;   /* this chunk's */
    const uint8_t* data;
} WatchChunk;

/* One FRAMES datagram: count packed frames, match frame first onwards.
 * records points at count * WATCH_FRAME_SIZE bytes. */
typedef struct WatchFrames {
    uint32_t cookie;
    uint32_t match_seq;
    int32_t first;
    uint8_t count;
    const uint8_t* records;
} WatchFrames;

typedef struct WatchAck {
    uint32_t cookie;
    uint32_t match_seq;
    uint8_t flags;      /* WATCH_ACK_* */
    int32_t contiguous; /* newest frame held with every one before it, -1 none */
    uint16_t buffered;  /* frames held the viewer has not run yet */
} WatchAck;

typedef struct WatchMatchEnd {
    uint32_t cookie;
    uint32_t match_seq;
    int32_t last;   /* the match's last frame */
    uint8_t reason; /* WATCH_END_* */
} WatchMatchEnd;

/* A header being put back together from its chunks, in any order. */
typedef struct WatchAssembly {
    uint32_t match_seq;
    uint16_t total; /* 0: no chunk yet */
    uint8_t cnt;
    uint8_t have; /* bit i: chunk i is in */
    uint8_t data[WATCH_HEADER_CHUNKS_MAX * WATCH_CHUNK_MAX];
} WatchAssembly;

/* The message type of a datagram of our protocol (WATCH_MSG_*),
 * WATCH_MSG_OTHER_PROTO for a watch datagram of another protocol, -1 for
 * anything else. */
int watch_msg_type(const uint8_t* in, size_t len);

/* The packers return the bytes written, 0 when the message does not fit
 * `cap` or is out of range. The unpackers return false for anything but
 * an exact, well-formed message of their type. */
size_t watch_pack_hello(uint8_t* out, size_t cap, const WatchHello* m);
bool watch_unpack_hello(const uint8_t* in, size_t len, WatchHello* m);
size_t watch_pack_welcome(uint8_t* out, size_t cap, const WatchWelcome* m);
bool watch_unpack_welcome(const uint8_t* in, size_t len, WatchWelcome* m);
size_t watch_pack_status(uint8_t* out, size_t cap, const WatchStatus* m);
bool watch_unpack_status(const uint8_t* in, size_t len, WatchStatus* m);
size_t watch_pack_ack(uint8_t* out, size_t cap, const WatchAck* m);
bool watch_unpack_ack(const uint8_t* in, size_t len, WatchAck* m);
size_t watch_pack_match_end(uint8_t* out, size_t cap, const WatchMatchEnd* m);
bool watch_unpack_match_end(const uint8_t* in, size_t len, WatchMatchEnd* m);
size_t watch_pack_bye(uint8_t* out, size_t cap, uint32_t cookie);
bool watch_unpack_bye(const uint8_t* in, size_t len, uint32_t* cookie);

/* FRAMES: 1 to WATCH_FRAMES_MAX packed records. */
size_t watch_pack_frames(uint8_t* out, size_t cap, const WatchFrames* m);
bool watch_unpack_frames(const uint8_t* in, size_t len, WatchFrames* m);

void watch_frame_pack(uint8_t out[WATCH_FRAME_SIZE], const WatchFrame* f);
void watch_frame_unpack(const uint8_t in[WATCH_FRAME_SIZE], WatchFrame* f);

/* The header: WATCH_HEADER_SIZE bytes, or 0 / false. Unpacking refuses
 * another WATCH_HEADER_VERSION. */
size_t watch_header_pack(uint8_t* out, size_t cap, const WatchHeader* h);
bool watch_header_unpack(const uint8_t* in, size_t len, WatchHeader* h);

/* Chunks a header of `total` bytes travels in; 0 when it is empty or too
 * long for WATCH_HEADER_CHUNKS_MAX chunks. */
int watch_chunk_count(size_t total);
/* HEADER datagram `idx` of the header at `header`. */
size_t watch_pack_chunk(uint8_t* out, size_t cap, uint32_t cookie, uint32_t match_seq,
    const uint8_t* header, size_t total, int idx);
bool watch_unpack_chunk(const uint8_t* in, size_t len, WatchChunk* c);

void watch_assembly_reset(WatchAssembly* a, uint32_t match_seq);
/* False for a chunk of another match, or one whose total or count disagrees
 * with the chunks already in; a repeat of one already in is accepted. */
bool watch_assembly_add(WatchAssembly* a, const WatchChunk* c);
bool watch_assembly_done(const WatchAssembly* a);

#ifdef __cplusplus
}
#endif

#endif
