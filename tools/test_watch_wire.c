/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Xelee Watch's wire codec (src/pc/net_watch_wire.c): the match header round
 * trip and its layout, frame packing and the 20-frame cap, header chunks put
 * back together out of order, the control messages, and the refusal of
 * truncated, padded and malformed datagrams. */
#include "pc/net_watch_wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void fill_pad_status(WatchPadStatus* s, int port, int which) {
    s->button = 0x80000000u | (uint32_t)(port * 16 + which);
    s->last_button = 0x00010000u + (uint32_t)port;
    s->trigger = 0x100u << which;
    s->repeat = 0x7u;
    s->release = 0xFFFFFFFFu - (uint32_t)port;
    s->repeat_count = -(port + which + 1);
    s->stick_x = (int8_t)(-80 + port);
    s->stick_y = (int8_t)(80 - which);
    s->substick_x = (int8_t)-128;
    s->substick_y = 127;
    s->analog_l = (uint8_t)(200 + port);
    s->analog_r = (uint8_t)(10 + which);
    s->analog_a = 0;
    s->analog_b = 255;
    for (int i = 0; i < 8; i++) {
        s->nml[i] = 0x3F800000u + (uint32_t)(i + port + which); /* 1.0f and its neighbours */
    }
    s->cross_dir = (uint8_t)(port + which);
    s->err = (int8_t)(which - 1);
}

static void fill_header(WatchHeader* h) {
    memset(h, 0, sizeof *h);
    h->version = WATCH_HEADER_VERSION;
    h->match_seq = 0x01020304u;
    h->host_mode = 2;  /* GM_VS */
    h->scene_kind = 3; /* GS_SUDDEN_DEATH */
    h->first_frame = 123456;
    h->seed = 0xDEADBEEFu;
    for (int i = 0; i < WATCH_INFO_SIZE; i++) {
        h->info[i] = (uint8_t)(i * 7 + 1);
    }
    for (int p = 0; p < 4; p++) {
        fill_pad_status(&h->pad_master[p], p, 0);
        fill_pad_status(&h->pad_copy[p], p, 1);
        fill_pad_status(&h->pad_game[p], p, 2);
        h->raw_master[p].x = (int8_t)-p;
        h->raw_master[p].y = (int8_t)p;
        h->raw_master[p].cx = (int8_t)-127;
        h->raw_master[p].cy = 127;
        h->raw_game[p].x = (int8_t)(p * 3);
        h->raw_game[p].y = (int8_t)(-p * 3);
        h->raw_game[p].cx = 1;
        h->raw_game[p].cy = -1;
    }
    h->unlock_state = 0x0123456789ABCDEFull;
    h->unlock_all = 1;
    h->frozen_stadium = 1;
    for (int i = 0; i < WATCH_RULES_BYTES; i++) {
        h->rules[i] = (uint8_t)(0xA0 + i);
    }
    h->rules_x14 = -123456;
    h->item_freq = 0xFF;
    h->item_mask = 0xFEDCBA9876543210ull;
    h->stage_mask = 0x00FFFF01u;
    h->ucf = 1;
    h->slippi_fixes = 1;
    h->pal = 0;
    h->language = 1;
    strcpy(h->rev, "v0.1.0-watch.12345678901"); /* exactly WATCH_REV_LEN */
    strcpy(h->disc, "1a2b3c4d");
}

static void same_pad_status(const WatchPadStatus* a, const WatchPadStatus* b) {
    assert(a->button == b->button && a->last_button == b->last_button);
    assert(a->trigger == b->trigger && a->repeat == b->repeat && a->release == b->release);
    assert(a->repeat_count == b->repeat_count);
    assert(a->stick_x == b->stick_x && a->stick_y == b->stick_y);
    assert(a->substick_x == b->substick_x && a->substick_y == b->substick_y);
    assert(a->analog_l == b->analog_l && a->analog_r == b->analog_r);
    assert(a->analog_a == b->analog_a && a->analog_b == b->analog_b);
    assert(memcmp(a->nml, b->nml, sizeof a->nml) == 0);
    assert(a->cross_dir == b->cross_dir && a->err == b->err);
}

static void test_header(void) {
    WatchHeader h, back;
    uint8_t buf[WATCH_HEADER_SIZE + 8], again[WATCH_HEADER_SIZE];
    fill_header(&h);
    assert(watch_header_pack(buf, sizeof buf, &h) == WATCH_HEADER_SIZE);
    assert(watch_header_pack(buf, WATCH_HEADER_SIZE - 1, &h) == 0); /* no room */

    /* The layout the spec gives: big-endian fields at fixed offsets. */
    assert(buf[0] == 0 && buf[1] == WATCH_HEADER_VERSION);
    assert(be32(buf + 2) == 0x01020304u && buf[6] == 2 && buf[7] == 3);
    assert(be32(buf + 8) == 123456u && be32(buf + 12) == 0xDEADBEEFu);
    assert(buf[16] == 1 && buf[17] == 8);   /* info[0], info[1] */
    assert(be32(buf + 328) == 0x80000000u); /* pad_master[0].button */
    assert(be32(buf + 1152) == 0x01234567u && be32(buf + 1156) == 0x89ABCDEFu);
    assert(buf[1162] == 0xA0 && buf[1181] == 0xB3);
    assert(be32(buf + 1182) == (uint32_t)-123456);
    assert(memcmp(buf + 1203, "v0.1.0-watch.12345678901", WATCH_REV_LEN) == 0);
    assert(memcmp(buf + 1227, "1a2b3c4d", WATCH_DISC_LEN) == 0);

    assert(watch_header_unpack(buf, WATCH_HEADER_SIZE, &back));
    assert(back.version == h.version && back.match_seq == h.match_seq);
    assert(back.host_mode == h.host_mode && back.scene_kind == h.scene_kind);
    assert(back.first_frame == h.first_frame && back.seed == h.seed);
    assert(memcmp(back.info, h.info, WATCH_INFO_SIZE) == 0);
    for (int p = 0; p < 4; p++) {
        same_pad_status(&back.pad_master[p], &h.pad_master[p]);
        same_pad_status(&back.pad_copy[p], &h.pad_copy[p]);
        same_pad_status(&back.pad_game[p], &h.pad_game[p]);
        assert(memcmp(&back.raw_master[p], &h.raw_master[p], sizeof h.raw_master[p]) == 0);
        assert(memcmp(&back.raw_game[p], &h.raw_game[p], sizeof h.raw_game[p]) == 0);
    }
    assert(back.unlock_state == h.unlock_state && back.unlock_all == 1);
    assert(back.frozen_stadium == 1);
    assert(memcmp(back.rules, h.rules, WATCH_RULES_BYTES) == 0 && back.rules_x14 == -123456);
    assert(back.item_freq == 0xFF && back.item_mask == h.item_mask);
    assert(back.stage_mask == h.stage_mask);
    assert(back.ucf == 1 && back.slippi_fixes == 1 && back.pal == 0 && back.language == 1);
    assert(strcmp(back.rev, h.rev) == 0 && strcmp(back.disc, h.disc) == 0);
    /* A full-width rev travels with no NUL and comes back terminated. */
    assert(strlen(back.rev) == WATCH_REV_LEN);
    /* Whatever the packer wrote, the unpacker read: the bytes come back. */
    assert(watch_header_pack(again, sizeof again, &back) == WATCH_HEADER_SIZE);
    assert(memcmp(again, buf, WATCH_HEADER_SIZE) == 0);

    /* Truncated, padded, another version: refused. */
    assert(!watch_header_unpack(buf, WATCH_HEADER_SIZE - 1, &back));
    buf[WATCH_HEADER_SIZE] = 0;
    assert(!watch_header_unpack(buf, WATCH_HEADER_SIZE + 1, &back));
    buf[1] = WATCH_HEADER_VERSION + 1;
    assert(!watch_header_unpack(buf, WATCH_HEADER_SIZE, &back));
    assert(!watch_header_unpack(NULL, 0, &back));
}

static void fill_frame(WatchFrame* f, int n) {
    memset(f, 0, sizeof *f);
    for (int p = 0; p < 4; p++) {
        WatchPad* pad = &f->pads[p];
        pad->button = (uint16_t)(0x1000 + n * 4 + p);
        pad->stick_x = (int8_t)(n - 64);
        pad->stick_y = (int8_t)(64 - n);
        pad->substick_x = (int8_t)-128;
        pad->substick_y = 127;
        pad->trigger_l = (uint8_t)(n * 3);
        pad->trigger_r = (uint8_t)(255 - n);
        pad->analog_a = (uint8_t)p;
        pad->analog_b = (uint8_t)(p * 2);
        pad->err = p == 3 ? (int8_t)-1 : 0;
    }
    f->ck = 0xC0DE0000u + (uint32_t)n;
    f->seed = 0x5EED0000u ^ (uint32_t)n;
    f->scene = (uint8_t)(n & 1 ? 2 : 3);
}

static void same_frame(const WatchFrame* a, const WatchFrame* b) {
    for (int p = 0; p < 4; p++) {
        const WatchPad* x = &a->pads[p];
        const WatchPad* y = &b->pads[p];
        assert(x->button == y->button && x->stick_x == y->stick_x && x->stick_y == y->stick_y);
        assert(x->substick_x == y->substick_x && x->substick_y == y->substick_y);
        assert(x->trigger_l == y->trigger_l && x->trigger_r == y->trigger_r);
        assert(x->analog_a == y->analog_a && x->analog_b == y->analog_b && x->err == y->err);
    }
    assert(a->ck == b->ck && a->seed == b->seed && a->scene == b->scene);
}

static void test_frames(void) {
    WatchFrame frames[WATCH_FRAMES_MAX + 1], back;
    uint8_t records[(WATCH_FRAMES_MAX + 1) * WATCH_FRAME_SIZE];
    uint8_t buf[WATCH_DATAGRAM_MAX + 64];
    for (int i = 0; i <= WATCH_FRAMES_MAX; i++) {
        fill_frame(&frames[i], i);
        watch_frame_pack(records + i * WATCH_FRAME_SIZE, &frames[i]);
        watch_frame_unpack(records + i * WATCH_FRAME_SIZE, &back);
        same_frame(&back, &frames[i]);
    }
    /* The record's layout: pads, then ck, seed and scene. */
    assert(records[0] == 0x10 && records[1] == 0x00);
    assert(be32(records + 44) == 0xC0DE0000u && be32(records + 48) == 0x5EED0000u);
    assert(records[52] == 3);

    WatchFrames m = {0xC00C1Eu, 9, 4000, WATCH_FRAMES_MAX, records};
    const size_t full = WATCH_FRAMES_HEAD + WATCH_FRAMES_MAX * WATCH_FRAME_SIZE;
    assert(full <= WATCH_DATAGRAM_MAX);
    assert(watch_pack_frames(buf, sizeof buf, &m) == full);
    assert(watch_msg_type(buf, full) == WATCH_MSG_FRAMES);

    WatchFrames got;
    assert(watch_unpack_frames(buf, full, &got));
    assert(got.cookie == 0xC00C1Eu && got.match_seq == 9 && got.first == 4000);
    assert(got.count == WATCH_FRAMES_MAX && got.records == buf + WATCH_FRAMES_HEAD);
    for (int i = 0; i < got.count; i++) {
        watch_frame_unpack(got.records + i * WATCH_FRAME_SIZE, &back);
        same_frame(&back, &frames[i]);
    }

    /* The cap: 21 frames and 0 frames do not pack. */
    m.count = WATCH_FRAMES_MAX + 1;
    assert(watch_pack_frames(buf, sizeof buf, &m) == 0);
    m.count = 0;
    assert(watch_pack_frames(buf, sizeof buf, &m) == 0);
    m.count = 3;
    m.first = -1;
    assert(watch_pack_frames(buf, sizeof buf, &m) == 0);
    m.first = 0;
    const size_t three = WATCH_FRAMES_HEAD + 3 * WATCH_FRAME_SIZE;
    assert(watch_pack_frames(buf, three - 1, &m) == 0); /* no room */
    assert(watch_pack_frames(buf, sizeof buf, &m) == three);
    assert(watch_unpack_frames(buf, three, &got) && got.count == 3);

    /* Truncated and padded: refused. */
    assert(!watch_unpack_frames(buf, three - 1, &got));
    assert(!watch_unpack_frames(buf, three + 1, &got));
    /* A count the length does not carry, and one over the cap. */
    buf[WATCH_FRAMES_HEAD - 1] = 4;
    assert(!watch_unpack_frames(buf, three, &got));
    buf[WATCH_FRAMES_HEAD - 1] = 0;
    assert(!watch_unpack_frames(buf, WATCH_FRAMES_HEAD, &got));
    assert(watch_pack_frames(buf, sizeof buf, &(WatchFrames){1, 2, 0, WATCH_FRAMES_MAX, records}) ==
           full);
    buf[WATCH_FRAMES_HEAD - 1] = WATCH_FRAMES_MAX + 1;
    assert(!watch_unpack_frames(buf, full, &got));
    /* A negative first frame. */
    assert(watch_pack_frames(buf, sizeof buf, &(WatchFrames){1, 2, 0, 1, records}) ==
           WATCH_FRAMES_HEAD + WATCH_FRAME_SIZE);
    buf[12] = 0x80;
    assert(!watch_unpack_frames(buf, WATCH_FRAMES_HEAD + WATCH_FRAME_SIZE, &got));
}

static void test_chunks(void) {
    WatchHeader h;
    uint8_t header[WATCH_HEADER_SIZE];
    uint8_t c0[WATCH_DATAGRAM_MAX], c1[WATCH_DATAGRAM_MAX], c2[WATCH_DATAGRAM_MAX];
    fill_header(&h);
    assert(watch_header_pack(header, sizeof header, &h) == WATCH_HEADER_SIZE);
    assert(watch_chunk_count(WATCH_HEADER_SIZE) == 2);
    assert(watch_chunk_count(0) == 0 && watch_chunk_count(WATCH_CHUNK_MAX) == 1);
    assert(watch_chunk_count(WATCH_HEADER_CHUNKS_MAX * WATCH_CHUNK_MAX + 1) == 0);

    const size_t n0 = watch_pack_chunk(c0, sizeof c0, 7, 42, header, WATCH_HEADER_SIZE, 0);
    const size_t n1 = watch_pack_chunk(c1, sizeof c1, 7, 42, header, WATCH_HEADER_SIZE, 1);
    assert(n0 == WATCH_CHUNK_HEAD + WATCH_CHUNK_MAX && n0 <= WATCH_DATAGRAM_MAX);
    assert(n1 == WATCH_CHUNK_HEAD + WATCH_HEADER_SIZE - WATCH_CHUNK_MAX);
    assert(watch_pack_chunk(c2, sizeof c2, 7, 42, header, WATCH_HEADER_SIZE, 2) == 0);
    assert(watch_pack_chunk(c2, sizeof c2, 7, 42, header, WATCH_HEADER_SIZE, -1) == 0);

    WatchChunk k0, k1;
    assert(watch_unpack_chunk(c0, n0, &k0) && watch_unpack_chunk(c1, n1, &k1));
    assert(k0.cookie == 7 && k0.match_seq == 42 && k0.idx == 0 && k0.cnt == 2);
    assert(k0.total == WATCH_HEADER_SIZE && k0.len == WATCH_CHUNK_MAX);
    assert(k1.idx == 1 && k1.len == WATCH_HEADER_SIZE - WATCH_CHUNK_MAX);

    /* Out of order, with a repeat in between. */
    WatchAssembly a;
    watch_assembly_reset(&a, 42);
    assert(!watch_assembly_done(&a));
    assert(watch_assembly_add(&a, &k1) && !watch_assembly_done(&a));
    assert(watch_assembly_add(&a, &k1) && !watch_assembly_done(&a));
    assert(watch_assembly_add(&a, &k0) && watch_assembly_done(&a));
    assert(a.total == WATCH_HEADER_SIZE && memcmp(a.data, header, WATCH_HEADER_SIZE) == 0);
    WatchHeader back;
    assert(watch_header_unpack(a.data, a.total, &back) && back.seed == h.seed);

    /* Another match's chunk, and one that disagrees about the size. */
    watch_assembly_reset(&a, 43);
    assert(!watch_assembly_add(&a, &k0));
    watch_assembly_reset(&a, 42);
    assert(watch_assembly_add(&a, &k0));
    uint8_t small[600];
    memset(small, 0x5A, sizeof small);
    const size_t ns = watch_pack_chunk(c2, sizeof c2, 7, 42, small, sizeof small, 0);
    WatchChunk ks;
    assert(ns == WATCH_CHUNK_HEAD + sizeof small && watch_unpack_chunk(c2, ns, &ks));
    assert(!watch_assembly_add(&a, &ks) && !watch_assembly_done(&a));

    /* Truncated, padded, an index past the count, a wrong count. */
    assert(!watch_unpack_chunk(c1, n1 - 1, &k1));
    c1[n1] = 0;
    assert(!watch_unpack_chunk(c1, n1 + 1, &k1));
    c1[12] = 2; /* idx */
    assert(!watch_unpack_chunk(c1, n1, &k1));
    c1[12] = 1;
    c1[13] = 3; /* cnt */
    assert(!watch_unpack_chunk(c1, n1, &k1));
    c1[13] = 2;
    assert(watch_unpack_chunk(c1, n1, &k1));
}

static void test_control(void) {
    uint8_t buf[256];

    WatchHello hello = {0x1122334455667788ull, 0xABCDEF01u, "v0.1.0-watch.7", "1a2b3c4d",
        "Quest3-livingrm1"}; /* a name of exactly WATCH_NAME_LEN */
    assert(watch_pack_hello(buf, sizeof buf, &hello) == WATCH_HELLO_SIZE);
    assert(watch_msg_type(buf, WATCH_HELLO_SIZE) == WATCH_MSG_HELLO);
    WatchHello hb;
    assert(watch_unpack_hello(buf, WATCH_HELLO_SIZE, &hb));
    assert(hb.viewer_id == hello.viewer_id && hb.nonce == hello.nonce);
    assert(strcmp(hb.rev, "v0.1.0-watch.7") == 0 && strcmp(hb.disc, "1a2b3c4d") == 0);
    assert(strlen(hb.name) == WATCH_NAME_LEN && strcmp(hb.name, hello.name) == 0);
    /* The text fields are NUL padded: the short rev ends in zeros. */
    assert(buf[16 + 14] == 0 && buf[16 + WATCH_REV_LEN - 1] == 0);
    assert(!watch_unpack_hello(buf, WATCH_HELLO_SIZE - 1, &hb));
    assert(!watch_unpack_hello(buf, WATCH_HELLO_SIZE + 1, &hb));

    WatchWelcome wel = {
        0xABCDEF01u, 0x0BADF00Du, 1, WATCH_REFUSE_NONE, WATCH_STATE_LIVE, 2, "Quest 3"};
    assert(watch_pack_welcome(buf, sizeof buf, &wel) == WATCH_WELCOME_SIZE);
    WatchWelcome wb;
    assert(watch_unpack_welcome(buf, WATCH_WELCOME_SIZE, &wb));
    assert(wb.nonce == wel.nonce && wb.cookie == wel.cookie && wb.ok == 1);
    assert(wb.reason == WATCH_REFUSE_NONE && wb.state == WATCH_STATE_LIVE && wb.mode == 2);
    assert(strcmp(wb.name, "Quest 3") == 0);
    /* The right length but another type is refused too. */
    assert(!watch_unpack_hello(buf, WATCH_WELCOME_SIZE, &hb));
    assert(!watch_unpack_welcome(buf, WATCH_WELCOME_SIZE - 1, &wb));

    WatchStatus st = {0x0BADF00Du, WATCH_STATE_UNWATCHABLE, 3, 12, -1};
    assert(watch_pack_status(buf, sizeof buf, &st) == WATCH_STATUS_SIZE);
    WatchStatus sb;
    assert(watch_unpack_status(buf, WATCH_STATUS_SIZE, &sb));
    assert(sb.cookie == st.cookie && sb.state == st.state && sb.mode == 3);
    assert(sb.match_seq == 12 && sb.frame == -1);
    assert(!watch_unpack_status(buf, WATCH_STATUS_SIZE + 1, &sb));

    WatchAck ack = {0x0BADF00Du, 12, WATCH_ACK_HEADER | WATCH_ACK_END, 3599, 240};
    assert(watch_pack_ack(buf, sizeof buf, &ack) == WATCH_ACK_SIZE);
    WatchAck ab;
    assert(watch_unpack_ack(buf, WATCH_ACK_SIZE, &ab));
    assert(ab.cookie == ack.cookie && ab.match_seq == 12 && ab.flags == 3);
    assert(ab.contiguous == 3599 && ab.buffered == 240);
    ack.contiguous = -2; /* below "none" */
    assert(watch_pack_ack(buf, sizeof buf, &ack) == WATCH_ACK_SIZE);
    assert(!watch_unpack_ack(buf, WATCH_ACK_SIZE, &ab));

    WatchMatchEnd end = {0x0BADF00Du, 12, 3599, WATCH_END_SCENE};
    assert(watch_pack_match_end(buf, sizeof buf, &end) == WATCH_MATCH_END_SIZE);
    WatchMatchEnd eb;
    assert(watch_unpack_match_end(buf, WATCH_MATCH_END_SIZE, &eb));
    assert(eb.cookie == end.cookie && eb.match_seq == 12 && eb.last == 3599 && eb.reason == 0);
    assert(!watch_unpack_match_end(buf, WATCH_MATCH_END_SIZE - 1, &eb));

    uint32_t cookie = 0;
    assert(watch_pack_bye(buf, sizeof buf, 0x0BADF00Du) == WATCH_BYE_SIZE);
    assert(watch_unpack_bye(buf, WATCH_BYE_SIZE, &cookie) && cookie == 0x0BADF00Du);
    assert(!watch_unpack_bye(buf, WATCH_BYE_SIZE + 1, &cookie));
    assert(watch_pack_bye(buf, WATCH_BYE_SIZE - 1, 1) == 0); /* no room */

    /* Not ours: another magic, another protocol, too short. */
    assert(watch_msg_type(buf, 3) == -1);
    buf[2] = WATCH_PROTO + 1;
    assert(watch_msg_type(buf, WATCH_BYE_SIZE) == WATCH_MSG_OTHER_PROTO);
    assert(!watch_unpack_bye(buf, WATCH_BYE_SIZE, &cookie));
    buf[0] = 'M';
    assert(watch_msg_type(buf, WATCH_BYE_SIZE) == -1);
}

int main(void) {
    test_header();
    test_frames();
    test_chunks();
    test_control();
    puts("PASS: watch header round trip and layout, frames and the 20-frame cap, chunks out of "
         "order, control messages, malformed datagrams refused");
    return 0;
}
