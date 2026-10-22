/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Xelee Watch's wire codec; the format is described in net_watch_wire.h. */
#include "pc/net_watch_wire.h"

#include <string.h>

/* ---- cursors ------------------------------------------------------------
 * A writer and a reader that never step past their end: the first overrun
 * clears ok and every later access does nothing, so a message is checked
 * once, when it is finished, rather than at every field. */
typedef struct Put {
    uint8_t* p;
    size_t n, cap;
    bool ok;
} Put;

typedef struct Get {
    const uint8_t* p;
    size_t n, len;
    bool ok;
} Get;

static Put put_begin(uint8_t* out, size_t cap) {
    Put w = {out, 0, cap, out != NULL};
    return w;
}

static bool put_room(Put* w, size_t n) {
    if (!w->ok || w->cap - w->n < n) {
        w->ok = false;
        return false;
    }
    return true;
}

static void put_u8(Put* w, uint8_t v) {
    if (put_room(w, 1)) {
        w->p[w->n++] = v;
    }
}

static void put_i8(Put* w, int8_t v) {
    put_u8(w, (uint8_t)v);
}

static void put_u16(Put* w, uint16_t v) {
    put_u8(w, (uint8_t)(v >> 8));
    put_u8(w, (uint8_t)v);
}

static void put_u32(Put* w, uint32_t v) {
    put_u16(w, (uint16_t)(v >> 16));
    put_u16(w, (uint16_t)v);
}

static void put_i32(Put* w, int32_t v) {
    put_u32(w, (uint32_t)v);
}

static void put_u64(Put* w, uint64_t v) {
    put_u32(w, (uint32_t)(v >> 32));
    put_u32(w, (uint32_t)v);
}

static void put_bytes(Put* w, const void* src, size_t n) {
    if (put_room(w, n)) {
        memcpy(w->p + w->n, src, n);
        w->n += n;
    }
}

/* A text field: the first `width` bytes of s at most, NUL padded. */
static void put_text(Put* w, const char* s, size_t width) {
    size_t len = 0;
    while (len < width && s[len] != '\0') {
        len++;
    }
    if (put_room(w, width)) {
        memcpy(w->p + w->n, s, len);
        memset(w->p + w->n + len, 0, width - len);
        w->n += width;
    }
}

static void put_prefix(Put* w, uint8_t type) {
    put_u8(w, 'X');
    put_u8(w, 'W');
    put_u8(w, WATCH_PROTO);
    put_u8(w, type);
}

static size_t put_end(const Put* w) {
    return w->ok ? w->n : 0;
}

static Get get_begin(const uint8_t* in, size_t len) {
    Get r = {in, 0, len, in != NULL};
    return r;
}

static bool get_room(Get* r, size_t n) {
    if (!r->ok || r->len - r->n < n) {
        r->ok = false;
        return false;
    }
    return true;
}

static uint8_t get_u8(Get* r) {
    return get_room(r, 1) ? r->p[r->n++] : 0;
}

static int8_t get_i8(Get* r) {
    return (int8_t)get_u8(r);
}

static uint16_t get_u16(Get* r) {
    uint16_t hi = get_u8(r);
    return (uint16_t)(hi << 8 | get_u8(r));
}

static uint32_t get_u32(Get* r) {
    uint32_t hi = get_u16(r);
    return hi << 16 | get_u16(r);
}

static int32_t get_i32(Get* r) {
    return (int32_t)get_u32(r);
}

static uint64_t get_u64(Get* r) {
    uint64_t hi = get_u32(r);
    return hi << 32 | get_u32(r);
}

static void get_bytes(Get* r, void* dst, size_t n) {
    if (get_room(r, n)) {
        memcpy(dst, r->p + r->n, n);
        r->n += n;
    } else {
        memset(dst, 0, n);
    }
}

/* Into a buffer of width + 1 bytes, always terminated. */
static void get_text(Get* r, char* dst, size_t width) {
    get_bytes(r, dst, width);
    dst[width] = '\0';
}

/* The prefix of a datagram of our protocol and of `type`, else not ok. */
static bool get_prefix(Get* r, uint8_t type) {
    uint8_t m0 = get_u8(r);
    uint8_t m1 = get_u8(r);
    uint8_t proto = get_u8(r);
    uint8_t t = get_u8(r);
    if (m0 != 'X' || m1 != 'W' || proto != WATCH_PROTO || t != type) {
        r->ok = false;
    }
    return r->ok;
}

/* Everything read, and nothing left over. */
static bool get_end(const Get* r) {
    return r->ok && r->n == r->len;
}

int watch_msg_type(const uint8_t* in, size_t len) {
    if (in == NULL || len < WATCH_PREFIX || in[0] != 'X' || in[1] != 'W') {
        return -1;
    }
    if (in[2] != WATCH_PROTO) {
        return WATCH_MSG_OTHER_PROTO;
    }
    return in[3];
}

/* ---- pads and frames --------------------------------------------------- */

static void put_pad(Put* w, const WatchPad* p) {
    put_u16(w, p->button);
    put_i8(w, p->stick_x);
    put_i8(w, p->stick_y);
    put_i8(w, p->substick_x);
    put_i8(w, p->substick_y);
    put_u8(w, p->trigger_l);
    put_u8(w, p->trigger_r);
    put_u8(w, p->analog_a);
    put_u8(w, p->analog_b);
    put_i8(w, p->err);
}

static void get_pad(Get* r, WatchPad* p) {
    p->button = get_u16(r);
    p->stick_x = get_i8(r);
    p->stick_y = get_i8(r);
    p->substick_x = get_i8(r);
    p->substick_y = get_i8(r);
    p->trigger_l = get_u8(r);
    p->trigger_r = get_u8(r);
    p->analog_a = get_u8(r);
    p->analog_b = get_u8(r);
    p->err = get_i8(r);
}

void watch_frame_pack(uint8_t out[WATCH_FRAME_SIZE], const WatchFrame* f) {
    Put w = put_begin(out, WATCH_FRAME_SIZE);
    for (int i = 0; i < 4; i++) {
        put_pad(&w, &f->pads[i]);
    }
    put_u32(&w, f->ck);
    put_u32(&w, f->seed);
    put_u8(&w, f->scene);
}

void watch_frame_unpack(const uint8_t in[WATCH_FRAME_SIZE], WatchFrame* f) {
    Get r = get_begin(in, WATCH_FRAME_SIZE);
    memset(f, 0, sizeof *f);
    for (int i = 0; i < 4; i++) {
        get_pad(&r, &f->pads[i]);
    }
    f->ck = get_u32(&r);
    f->seed = get_u32(&r);
    f->scene = get_u8(&r);
}

/* ---- the match header -------------------------------------------------- */

static void put_pad_status(Put* w, const WatchPadStatus* s) {
    put_u32(w, s->button);
    put_u32(w, s->last_button);
    put_u32(w, s->trigger);
    put_u32(w, s->repeat);
    put_u32(w, s->release);
    put_i32(w, s->repeat_count);
    put_i8(w, s->stick_x);
    put_i8(w, s->stick_y);
    put_i8(w, s->substick_x);
    put_i8(w, s->substick_y);
    put_u8(w, s->analog_l);
    put_u8(w, s->analog_r);
    put_u8(w, s->analog_a);
    put_u8(w, s->analog_b);
    for (int i = 0; i < 8; i++) {
        put_u32(w, s->nml[i]);
    }
    put_u8(w, s->cross_dir);
    put_i8(w, s->err);
}

static void get_pad_status(Get* r, WatchPadStatus* s) {
    s->button = get_u32(r);
    s->last_button = get_u32(r);
    s->trigger = get_u32(r);
    s->repeat = get_u32(r);
    s->release = get_u32(r);
    s->repeat_count = get_i32(r);
    s->stick_x = get_i8(r);
    s->stick_y = get_i8(r);
    s->substick_x = get_i8(r);
    s->substick_y = get_i8(r);
    s->analog_l = get_u8(r);
    s->analog_r = get_u8(r);
    s->analog_a = get_u8(r);
    s->analog_b = get_u8(r);
    for (int i = 0; i < 8; i++) {
        s->nml[i] = get_u32(r);
    }
    s->cross_dir = get_u8(r);
    s->err = get_i8(r);
}

static void put_sticks(Put* w, const WatchSticks* s) {
    put_i8(w, s->x);
    put_i8(w, s->y);
    put_i8(w, s->cx);
    put_i8(w, s->cy);
}

static void get_sticks(Get* r, WatchSticks* s) {
    s->x = get_i8(r);
    s->y = get_i8(r);
    s->cx = get_i8(r);
    s->cy = get_i8(r);
}

size_t watch_header_pack(uint8_t* out, size_t cap, const WatchHeader* h) {
    Put w = put_begin(out, cap);
    put_u16(&w, h->version);
    put_u32(&w, h->match_seq);
    put_u8(&w, h->host_mode);
    put_u8(&w, h->scene_kind);
    put_i32(&w, h->first_frame);
    put_u32(&w, h->seed);
    put_bytes(&w, h->info, WATCH_INFO_SIZE);
    for (int i = 0; i < 4; i++) {
        put_pad_status(&w, &h->pad_master[i]);
    }
    for (int i = 0; i < 4; i++) {
        put_pad_status(&w, &h->pad_copy[i]);
    }
    for (int i = 0; i < 4; i++) {
        put_pad_status(&w, &h->pad_game[i]);
    }
    for (int i = 0; i < 4; i++) {
        put_sticks(&w, &h->raw_master[i]);
    }
    for (int i = 0; i < 4; i++) {
        put_sticks(&w, &h->raw_game[i]);
    }
    put_u64(&w, h->unlock_state);
    put_u8(&w, h->unlock_all);
    put_u8(&w, h->frozen_stadium);
    put_bytes(&w, h->rules, WATCH_RULES_BYTES);
    put_i32(&w, h->rules_x14);
    put_u8(&w, h->item_freq);
    put_u64(&w, h->item_mask);
    put_u32(&w, h->stage_mask);
    put_u8(&w, h->ucf);
    put_u8(&w, h->slippi_fixes);
    put_u8(&w, h->pal);
    put_u8(&w, h->language);
    put_text(&w, h->rev, WATCH_REV_LEN);
    put_text(&w, h->disc, WATCH_DISC_LEN);
    return w.ok && w.n == WATCH_HEADER_SIZE ? w.n : 0;
}

bool watch_header_unpack(const uint8_t* in, size_t len, WatchHeader* h) {
    Get r = get_begin(in, len);
    memset(h, 0, sizeof *h);
    h->version = get_u16(&r);
    if (!r.ok || h->version != WATCH_HEADER_VERSION) {
        return false;
    }
    h->match_seq = get_u32(&r);
    h->host_mode = get_u8(&r);
    h->scene_kind = get_u8(&r);
    h->first_frame = get_i32(&r);
    h->seed = get_u32(&r);
    get_bytes(&r, h->info, WATCH_INFO_SIZE);
    for (int i = 0; i < 4; i++) {
        get_pad_status(&r, &h->pad_master[i]);
    }
    for (int i = 0; i < 4; i++) {
        get_pad_status(&r, &h->pad_copy[i]);
    }
    for (int i = 0; i < 4; i++) {
        get_pad_status(&r, &h->pad_game[i]);
    }
    for (int i = 0; i < 4; i++) {
        get_sticks(&r, &h->raw_master[i]);
    }
    for (int i = 0; i < 4; i++) {
        get_sticks(&r, &h->raw_game[i]);
    }
    h->unlock_state = get_u64(&r);
    h->unlock_all = get_u8(&r);
    h->frozen_stadium = get_u8(&r);
    get_bytes(&r, h->rules, WATCH_RULES_BYTES);
    h->rules_x14 = get_i32(&r);
    h->item_freq = get_u8(&r);
    h->item_mask = get_u64(&r);
    h->stage_mask = get_u32(&r);
    h->ucf = get_u8(&r);
    h->slippi_fixes = get_u8(&r);
    h->pal = get_u8(&r);
    h->language = get_u8(&r);
    get_text(&r, h->rev, WATCH_REV_LEN);
    get_text(&r, h->disc, WATCH_DISC_LEN);
    return get_end(&r);
}

/* ---- control messages -------------------------------------------------- */

size_t watch_pack_hello(uint8_t* out, size_t cap, const WatchHello* m) {
    Put w = put_begin(out, cap);
    put_prefix(&w, WATCH_MSG_HELLO);
    put_u64(&w, m->viewer_id);
    put_u32(&w, m->nonce);
    put_text(&w, m->rev, WATCH_REV_LEN);
    put_text(&w, m->disc, WATCH_DISC_LEN);
    put_text(&w, m->name, WATCH_NAME_LEN);
    return put_end(&w);
}

bool watch_unpack_hello(const uint8_t* in, size_t len, WatchHello* m) {
    Get r = get_begin(in, len);
    memset(m, 0, sizeof *m);
    get_prefix(&r, WATCH_MSG_HELLO);
    m->viewer_id = get_u64(&r);
    m->nonce = get_u32(&r);
    get_text(&r, m->rev, WATCH_REV_LEN);
    get_text(&r, m->disc, WATCH_DISC_LEN);
    get_text(&r, m->name, WATCH_NAME_LEN);
    return get_end(&r);
}

size_t watch_pack_welcome(uint8_t* out, size_t cap, const WatchWelcome* m) {
    Put w = put_begin(out, cap);
    put_prefix(&w, WATCH_MSG_WELCOME);
    put_u32(&w, m->nonce);
    put_u32(&w, m->cookie);
    put_u8(&w, m->ok);
    put_u8(&w, m->reason);
    put_u8(&w, m->state);
    put_u8(&w, m->mode);
    put_text(&w, m->name, WATCH_NAME_LEN);
    return put_end(&w);
}

bool watch_unpack_welcome(const uint8_t* in, size_t len, WatchWelcome* m) {
    Get r = get_begin(in, len);
    memset(m, 0, sizeof *m);
    get_prefix(&r, WATCH_MSG_WELCOME);
    m->nonce = get_u32(&r);
    m->cookie = get_u32(&r);
    m->ok = get_u8(&r);
    m->reason = get_u8(&r);
    m->state = get_u8(&r);
    m->mode = get_u8(&r);
    get_text(&r, m->name, WATCH_NAME_LEN);
    return get_end(&r);
}

size_t watch_pack_status(uint8_t* out, size_t cap, const WatchStatus* m) {
    Put w = put_begin(out, cap);
    put_prefix(&w, WATCH_MSG_STATUS);
    put_u32(&w, m->cookie);
    put_u8(&w, m->state);
    put_u8(&w, m->mode);
    put_u32(&w, m->match_seq);
    put_i32(&w, m->frame);
    return put_end(&w);
}

bool watch_unpack_status(const uint8_t* in, size_t len, WatchStatus* m) {
    Get r = get_begin(in, len);
    memset(m, 0, sizeof *m);
    get_prefix(&r, WATCH_MSG_STATUS);
    m->cookie = get_u32(&r);
    m->state = get_u8(&r);
    m->mode = get_u8(&r);
    m->match_seq = get_u32(&r);
    m->frame = get_i32(&r);
    return get_end(&r) && m->frame >= -1;
}

size_t watch_pack_ack(uint8_t* out, size_t cap, const WatchAck* m) {
    Put w = put_begin(out, cap);
    put_prefix(&w, WATCH_MSG_ACK);
    put_u32(&w, m->cookie);
    put_u32(&w, m->match_seq);
    put_u8(&w, m->flags);
    put_i32(&w, m->contiguous);
    put_u16(&w, m->buffered);
    return put_end(&w);
}

bool watch_unpack_ack(const uint8_t* in, size_t len, WatchAck* m) {
    Get r = get_begin(in, len);
    memset(m, 0, sizeof *m);
    get_prefix(&r, WATCH_MSG_ACK);
    m->cookie = get_u32(&r);
    m->match_seq = get_u32(&r);
    m->flags = get_u8(&r);
    m->contiguous = get_i32(&r);
    m->buffered = get_u16(&r);
    return get_end(&r) && m->contiguous >= -1;
}

size_t watch_pack_match_end(uint8_t* out, size_t cap, const WatchMatchEnd* m) {
    Put w = put_begin(out, cap);
    put_prefix(&w, WATCH_MSG_MATCH_END);
    put_u32(&w, m->cookie);
    put_u32(&w, m->match_seq);
    put_i32(&w, m->last);
    put_u8(&w, m->reason);
    return put_end(&w);
}

bool watch_unpack_match_end(const uint8_t* in, size_t len, WatchMatchEnd* m) {
    Get r = get_begin(in, len);
    memset(m, 0, sizeof *m);
    get_prefix(&r, WATCH_MSG_MATCH_END);
    m->cookie = get_u32(&r);
    m->match_seq = get_u32(&r);
    m->last = get_i32(&r);
    m->reason = get_u8(&r);
    return get_end(&r) && m->last >= -1;
}

size_t watch_pack_bye(uint8_t* out, size_t cap, uint32_t cookie) {
    Put w = put_begin(out, cap);
    put_prefix(&w, WATCH_MSG_BYE);
    put_u32(&w, cookie);
    return put_end(&w);
}

bool watch_unpack_bye(const uint8_t* in, size_t len, uint32_t* cookie) {
    Get r = get_begin(in, len);
    get_prefix(&r, WATCH_MSG_BYE);
    *cookie = get_u32(&r);
    return get_end(&r);
}

/* ---- frames ------------------------------------------------------------ */

size_t watch_pack_frames(uint8_t* out, size_t cap, const WatchFrames* m) {
    if (m->count == 0 || m->count > WATCH_FRAMES_MAX || m->records == NULL || m->first < 0) {
        return 0;
    }
    Put w = put_begin(out, cap);
    put_prefix(&w, WATCH_MSG_FRAMES);
    put_u32(&w, m->cookie);
    put_u32(&w, m->match_seq);
    put_i32(&w, m->first);
    put_u8(&w, m->count);
    put_bytes(&w, m->records, (size_t)m->count * WATCH_FRAME_SIZE);
    return put_end(&w);
}

bool watch_unpack_frames(const uint8_t* in, size_t len, WatchFrames* m) {
    Get r = get_begin(in, len);
    memset(m, 0, sizeof *m);
    get_prefix(&r, WATCH_MSG_FRAMES);
    m->cookie = get_u32(&r);
    m->match_seq = get_u32(&r);
    m->first = get_i32(&r);
    m->count = get_u8(&r);
    /* first + count must stay a frame number: no wrap past INT32_MAX. */
    if (!r.ok || m->count == 0 || m->count > WATCH_FRAMES_MAX || m->first < 0 ||
        m->first > INT32_MAX - WATCH_FRAMES_MAX ||
        r.len - r.n != (size_t)m->count * WATCH_FRAME_SIZE)
    {
        m->count = 0;
        return false;
    }
    m->records = r.p + r.n;
    return true;
}

/* ---- header chunks ----------------------------------------------------- */

int watch_chunk_count(size_t total) {
    if (total == 0 || total > (size_t)WATCH_HEADER_CHUNKS_MAX * WATCH_CHUNK_MAX) {
        return 0;
    }
    return (int)((total + WATCH_CHUNK_MAX - 1) / WATCH_CHUNK_MAX);
}

/* Bytes chunk idx of a header of `total` bytes carries (idx in range). */
static size_t chunk_len(size_t total, int idx) {
    size_t off = (size_t)idx * WATCH_CHUNK_MAX;
    return total - off < WATCH_CHUNK_MAX ? total - off : WATCH_CHUNK_MAX;
}

size_t watch_pack_chunk(uint8_t* out, size_t cap, uint32_t cookie, uint32_t match_seq,
    const uint8_t* header, size_t total, int idx) {
    const int cnt = watch_chunk_count(total);
    if (header == NULL || cnt == 0 || idx < 0 || idx >= cnt) {
        return 0;
    }
    Put w = put_begin(out, cap);
    put_prefix(&w, WATCH_MSG_HEADER);
    put_u32(&w, cookie);
    put_u32(&w, match_seq);
    put_u8(&w, (uint8_t)idx);
    put_u8(&w, (uint8_t)cnt);
    put_u16(&w, (uint16_t)total);
    put_bytes(&w, header + (size_t)idx * WATCH_CHUNK_MAX, chunk_len(total, idx));
    return put_end(&w);
}

bool watch_unpack_chunk(const uint8_t* in, size_t len, WatchChunk* c) {
    Get r = get_begin(in, len);
    memset(c, 0, sizeof *c);
    get_prefix(&r, WATCH_MSG_HEADER);
    c->cookie = get_u32(&r);
    c->match_seq = get_u32(&r);
    c->idx = get_u8(&r);
    c->cnt = get_u8(&r);
    c->total = get_u16(&r);
    if (!r.ok) {
        return false;
    }
    const int cnt = watch_chunk_count(c->total);
    if (cnt == 0 || c->cnt != cnt || c->idx >= cnt) {
        return false;
    }
    const size_t want = chunk_len(c->total, c->idx);
    if (r.len - r.n != want) {
        return false;
    }
    c->len = (uint16_t)want;
    c->data = r.p + r.n;
    return true;
}

void watch_assembly_reset(WatchAssembly* a, uint32_t match_seq) {
    a->match_seq = match_seq;
    a->total = 0;
    a->cnt = 0;
    a->have = 0;
}

bool watch_assembly_add(WatchAssembly* a, const WatchChunk* c) {
    const int cnt = watch_chunk_count(c->total);
    if (c->match_seq != a->match_seq || cnt == 0 || c->cnt != cnt || c->idx >= cnt ||
        c->data == NULL || c->len != chunk_len(c->total, c->idx))
    {
        return false;
    }
    if (a->total == 0) {
        a->total = c->total;
        a->cnt = c->cnt;
    } else if (c->total != a->total || c->cnt != a->cnt) {
        return false;
    }
    memcpy(a->data + (size_t)c->idx * WATCH_CHUNK_MAX, c->data, c->len);
    a->have |= (uint8_t)(1u << c->idx);
    return true;
}

bool watch_assembly_done(const WatchAssembly* a) {
    return a->total != 0 && a->have == (uint8_t)((1u << a->cnt) - 1u);
}
