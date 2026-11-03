// zlib's inflate_fast (main 0x1422ce8) and adler32 (main 0x1422774) as host code. The game's archives (.arc) are zlib
// streams, and decompressing them is most of a loading screen's work: on the loader thread (main 0xdc74e0) 80% of the
// time went to the game's inflate, 72% to inflate_fast alone (2026-10-31, quest 101's load, sampled); with it as host
// code, adler32 (the check of every stream's data) was the next 15%. Together: the loader thread's time from title
// to a quest -60%. The game's zlib is 1.2.8
// ("1.2.8.f-NINTENDO-SDK-v1"); this is zlib 1.2.8's inffast.c on the game's own structures, in guest memory:
// z_stream as zlib's (32-bit), inflate_state 8 bytes shorter at the front than stock 1.2.8 (window +0x2c, hold +0x30,
// bits +0x34, lencode +0x44, distcode +0x48, lenbits +0x4c, distbits +0x50), its modes TYPE 11 and BAD 27, no
// INFLATE_STRICT (dmax), and a distance too far back always an error (no `sane` test), as the game's function does.
// MHGU_INFLATE=game: the game's function; MHGU_INFLATE=check: both on every call, results compared (logged).
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#define FN_INFLATE_FAST 0x1422ce8u  // (z_stream *strm, unsigned start: avail_out when inflate() was entered)
#define FN_ADLER32      0x1422774u  // (adler, buf, len) -> adler

#define ZS_NEXT_IN   0x00u
#define ZS_AVAIL_IN  0x04u
#define ZS_NEXT_OUT  0x0cu
#define ZS_AVAIL_OUT 0x10u
#define ZS_MSG       0x18u
#define ZS_STATE     0x1cu

#define ST_MODE      0x00u
#define ST_WSIZE     0x20u
#define ST_WHAVE     0x24u
#define ST_WNEXT     0x28u
#define ST_WINDOW    0x2cu
#define ST_HOLD      0x30u
#define ST_BITS      0x34u
#define ST_LENCODE   0x44u
#define ST_DISTCODE  0x48u
#define ST_LENBITS   0x4cu
#define ST_DISTBITS  0x50u

#define MODE_TYPE 11u
#define MODE_BAD  27u

#define MSG_LITLEN   0x17860b5u  // "invalid literal/length code"
#define MSG_DISTCODE 0x17860d1u  // "invalid distance code"
#define MSG_DISTFAR  0x17860e7u  // "invalid distance too far back"

typedef struct { uint8_t op, bits; uint16_t val; } Code;  // zlib's `code`

static void inflate_fast(uint32_t strm, unsigned start) {
    uint32_t st = LD32(strm + ZS_STATE);
    const uint8_t *in = G2H(LD32(strm + ZS_NEXT_IN));
    const uint8_t *last = in + (LD32(strm + ZS_AVAIL_IN) - 5);  // (inflate() calls this with avail_in >= 6)
    uint8_t *out = G2H(LD32(strm + ZS_NEXT_OUT));
    unsigned avail_out = LD32(strm + ZS_AVAIL_OUT);
    uint8_t *beg = out - (start - avail_out);
    uint8_t *end = out + (avail_out - 257);                         // (and avail_out >= 258)
    unsigned wsize = LD32(st + ST_WSIZE), whave = LD32(st + ST_WHAVE), wnext = LD32(st + ST_WNEXT);
    const uint8_t *window = G2H(LD32(st + ST_WINDOW));
    uint32_t hold = LD32(st + ST_HOLD);
    unsigned bits = LD32(st + ST_BITS);
    const Code *lcode = G2H(LD32(st + ST_LENCODE)), *dcode = G2H(LD32(st + ST_DISTCODE));
    unsigned lmask = (1u << LD32(st + ST_LENBITS)) - 1, dmask = (1u << LD32(st + ST_DISTBITS)) - 1;
    unsigned op, len, dist;
    const uint8_t *from;
    Code here;

    do {
        if (bits < 15) {
            hold += (uint32_t)*in++ << bits;
            bits += 8;
            hold += (uint32_t)*in++ << bits;
            bits += 8;
        }
        here = lcode[hold & lmask];
    dolen:
        op = here.bits;
        hold >>= op;
        bits -= op;
        op = here.op;
        if (op == 0) {  // literal
            *out++ = (uint8_t)here.val;
        } else if (op & 16) {  // length base
            len = here.val;
            op &= 15;
            if (op) {
                if (bits < op) {
                    hold += (uint32_t)*in++ << bits;
                    bits += 8;
                }
                len += hold & ((1u << op) - 1);
                hold >>= op;
                bits -= op;
            }
            if (bits < 15) {
                hold += (uint32_t)*in++ << bits;
                bits += 8;
                hold += (uint32_t)*in++ << bits;
                bits += 8;
            }
            here = dcode[hold & dmask];
        dodist:
            op = here.bits;
            hold >>= op;
            bits -= op;
            op = here.op;
            if (op & 16) {  // distance base
                dist = here.val;
                op &= 15;
                if (bits < op) {
                    hold += (uint32_t)*in++ << bits;
                    bits += 8;
                    if (bits < op) {
                        hold += (uint32_t)*in++ << bits;
                        bits += 8;
                    }
                }
                dist += hold & ((1u << op) - 1);
                hold >>= op;
                bits -= op;
                op = (unsigned)(out - beg);  // max distance in output
                if (dist > op) {  // see if copy from window
                    op = dist - op;  // distance back in window
                    if (op > whave) {
                        ST32(strm + ZS_MSG, MSG_DISTFAR);
                        ST32(st + ST_MODE, MODE_BAD);
                        break;
                    }
                    from = window;
                    if (wnext == 0) {  // very common case
                        from += wsize - op;
                        if (op < len) {  // some from window
                            len -= op;
                            do *out++ = *from++; while (--op);
                            from = out - dist;  // rest from output
                        }
                    } else if (wnext < op) {  // wrap around window
                        from += wsize + wnext - op;
                        op -= wnext;
                        if (op < len) {  // some from end of window
                            len -= op;
                            do *out++ = *from++; while (--op);
                            from = window;
                            if (wnext < len) {  // some from start of window
                                op = wnext;
                                len -= op;
                                do *out++ = *from++; while (--op);
                                from = out - dist;  // rest from output
                            }
                        }
                    } else {  // contiguous in window
                        from += wnext - op;
                        if (op < len) {  // some from window
                            len -= op;
                            do *out++ = *from++; while (--op);
                            from = out - dist;  // rest from output
                        }
                    }
                    while (len > 2) {
                        *out++ = *from++;
                        *out++ = *from++;
                        *out++ = *from++;
                        len -= 3;
                    }
                    if (len) {
                        *out++ = *from++;
                        if (len > 1) *out++ = *from++;
                    }
                } else {
                    from = out - dist;  // copy direct from output
                    do {  // minimum length is three
                        *out++ = *from++;
                        *out++ = *from++;
                        *out++ = *from++;
                        len -= 3;
                    } while (len > 2);
                    if (len) {
                        *out++ = *from++;
                        if (len > 1) *out++ = *from++;
                    }
                }
            } else if ((op & 64) == 0) {  // 2nd level distance code
                here = dcode[here.val + (hold & ((1u << op) - 1))];
                goto dodist;
            } else {
                ST32(strm + ZS_MSG, MSG_DISTCODE);
                ST32(st + ST_MODE, MODE_BAD);
                break;
            }
        } else if ((op & 64) == 0) {  // 2nd level length code
            here = lcode[here.val + (hold & ((1u << op) - 1))];
            goto dolen;
        } else if (op & 32) {  // end-of-block
            ST32(st + ST_MODE, MODE_TYPE);
            break;
        } else {
            ST32(strm + ZS_MSG, MSG_LITLEN);
            ST32(st + ST_MODE, MODE_BAD);
            break;
        }
    } while (in < last && out < end);

    // return unused bytes (on entry, bits < 8, so in won't go too far back)
    len = bits >> 3;
    in -= len;
    bits -= len << 3;
    hold &= (1u << bits) - 1;
    ST32(strm + ZS_NEXT_IN, H2G(in));
    ST32(strm + ZS_NEXT_OUT, H2G(out));
    ST32(strm + ZS_AVAIL_IN, (uint32_t)(5 + (last - in)));
    ST32(strm + ZS_AVAIL_OUT, (uint32_t)(257 + (end - out)));
    ST32(st + ST_HOLD, hold);
    ST32(st + ST_BITS, bits);
}

// zlib's adler32: the same value for any input (as zlib, the running value's halves are taken as below 65521)
static uint32_t adler32(uint32_t adler, uint32_t buf, uint32_t len) {
    if (!buf) return 1;
    uint32_t a = adler & 0xffff, b = adler >> 16;
    const uint8_t *p = G2H(buf);
    while (len) {
        uint32_t n = len < 5552 ? len : 5552;  // (zlib's NMAX: b can't overflow before the reduction)
        len -= n;
        for (; n >= 16; n -= 16, p += 16)
            for (int i = 0; i < 16; i++) a += p[i], b += a;
        while (n--) a += *p++, b += a;
        a %= 65521, b %= 65521;
    }
    return b << 16 | a;
}

static void hook(Cpu *c) { inflate_fast(c->r[0], c->r[1]); }
static void hook_adler(Cpu *c) { c->r[0] = adler32(c->r[0], c->r[1], c->r[2]); }

// MHGU_INFLATE=check: the host version, then the game's on the same input; what they wrote compared
static void hook_check(Cpu *c) {
    static uint64_t calls, bad;
    uint32_t strm = c->r[0], start = c->r[1], st = LD32(strm + ZS_STATE);
    uint32_t out = LD32(strm + ZS_NEXT_OUT), n = LD32(strm + ZS_AVAIL_OUT);
    uint8_t zs0[0x20], st0[0x38], zs1[0x20], st1[0x38];
    memcpy(zs0, G2H(strm), sizeof zs0);
    memcpy(st0, G2H(st), sizeof st0);
    uint8_t *mine = malloc(n);
    inflate_fast(strm, start);
    memcpy(mine, G2H(out), n);
    memcpy(zs1, G2H(strm), sizeof zs1);
    memcpy(st1, G2H(st), sizeof st1);
    memcpy(G2H(strm), zs0, sizeof zs0);
    memcpy(G2H(st), st0, sizeof st0);
    fnhook_call_original(c, FN_INFLATE_FAST);
    calls++;
    if (memcmp(mine, G2H(out), n) || memcmp(zs1, G2H(strm), sizeof zs1) || memcmp(st1, G2H(st), sizeof st1)) {
        if (bad++ < 20) LOG("[inflate] check: call %llu differs (strm %08x)\n", (unsigned long long)calls, strm);
    }
    if ((calls & (calls - 1)) == 0 && calls >= 256)
        LOG("[inflate] check: %llu calls, %llu different\n", (unsigned long long)calls, (unsigned long long)bad);
    free(mine);
}
static void hook_adler_check(Cpu *c) {
    static uint64_t calls, bad;
    uint32_t in[3] = {c->r[0], c->r[1], c->r[2]}, mine = adler32(in[0], in[1], in[2]);
    fnhook_call_original(c, FN_ADLER32);
    calls++;
    if (mine != c->r[0] && bad++ < 20)
        LOG("[inflate] check: adler32(%08x, %08x, %u) = %08x here, %08x the game's\n", in[0], in[1], in[2], mine, c->r[0]);
    if ((calls & (calls - 1)) == 0 && calls >= 256)
        LOG("[inflate] check: %llu adler32 calls, %llu different\n", (unsigned long long)calls, (unsigned long long)bad);
}

void inflate_init(void) {
    const char *e = getenv("MHGU_INFLATE");
    if (e && !strcmp(e, "game")) return;
    bool check = e && !strcmp(e, "check");
    fnhook_set(FN_INFLATE_FAST, hle_register("inflate:inflate_fast", check ? hook_check : hook));
    fnhook_set(FN_ADLER32, hle_register("inflate:adler32", check ? hook_adler_check : hook_adler));
    if (check) LOG("[inflate] checking the host inflate_fast against the game's\n");
}
