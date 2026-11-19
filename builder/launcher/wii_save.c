/* From MildRain479/PokePark-2-Wiicompiled, recomp/boot/wii_save.c (CC0 1.0),
 * with the Wii data.bin reading (its AES code and the Wii's SD key) taken
 * out: Wonderpark doesn't ship that key. A save from a real Wii goes
 * through Dolphin first (Tools > Import Wii Save). The launcher's Extras >
 * Saves imports and exports with it (saves.cpp). Its platform hooks
 * (save_import, save_export, hle_nand_root) aren't used.
 *
 * Bringing over a save from Dolphin (wii_save.h): its save folder's files,
 * or a zip of them. */
#include "wii_save.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#define mkdir_one(p) _mkdir(p)
#define rmdir_one(p) _rmdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0770)
#define rmdir_one(p) rmdir(p)
#endif

const char* wiisave_message(int result) {
    switch (result) {
    case WIISAVE_OK: return "Imported";
    case WIISAVE_NOT_A_SAVE: return "Not a Wii save";
    case WIISAVE_OTHER_GAME: return "Another game's save";
    case WIISAVE_UNREADABLE: return "Couldn't read it";
    default: return "Couldn't save it";
    }
}

/* ------------------------------------------------------------------------ */
/* Reading a save */

/* A plain file name: no folders, nothing hidden or odd. */
static int plain_name(const char* s) {
    size_t i, n = strlen(s);
    if (n == 0 || n >= 64 || s[0] == '.') return 0;
    for (i = 0; i < n; i++)
        if (s[i] == '/' || s[i] == '\\' || s[i] == ':' || (unsigned char)s[i] < 0x20) return 0;
    return 1;
}

int wiisave_check_files(const WiiSaveFile* files, int count) {
    int i, game = 0;
    for (i = 0; i < count; i++) {
        const char* s = files[i].name;
        if (!plain_name(s)) return 0;
        if (s[0] == 'G' && s[1] == 'D' && s[2] >= '0' && s[2] <= '9' && (!s[3] || (s[3] == 'M' && !s[4]))) game = 1;
    }
    return game;
}

/* ------------------------------------------------------------------------ */
/* Putting it in place */

/* A folder of plain files, and the folder, deleted; 1 if it's gone. */
static int remove_dir(const char* dir) {
    char path[1024];
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(path, sizeof(path), "%s/*", dir);
    if ((h = FindFirstFileA(path, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(path, sizeof(path), "%s/%s", dir, fd.cFileName);
            remove(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            remove(path);
        }
        closedir(d);
    }
#endif
    return rmdir_one(dir) == 0 || errno == ENOENT;
}

static void make_dirs(const char* path) {
    char tmp[1024];
    size_t i;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            mkdir_one(tmp);
            tmp[i] = c;
        }
    }
    mkdir_one(tmp);
}

static int exists(const char* path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

int wiisave_install(const char* nand_root, const WiiSaveFile* files, int count) {
    char title[1024], dir[1100], kept[1100], fresh[1100], path[1200];
    int i;
    if (!wiisave_check_files(files, count)) return WIISAVE_NOT_A_SAVE;
    snprintf(title, sizeof(title), "%s/title/00010000/53324c45", nand_root);
    snprintf(dir, sizeof(dir), "%s/data", title);
    snprintf(kept, sizeof(kept), "%s/data before import", title);
    snprintf(fresh, sizeof(fresh), "%s/data importing", title);
    /* Written beside it first, so a failure leaves the save as it was. */
    remove_dir(fresh);
    make_dirs(fresh);
    for (i = 0; i < count; i++) {
        FILE* f;
        int ok;
        snprintf(path, sizeof(path), "%s/%s", fresh, files[i].name);
        if (!(f = fopen(path, "wb"))) break;
        ok = fwrite(files[i].data, 1, files[i].size, f) == files[i].size;
        if (fclose(f) != 0 || !ok) break;
    }
    if (i < count || !remove_dir(kept)) {
        remove_dir(fresh);
        return WIISAVE_UNWRITABLE;
    }
    if (exists(dir) && rename(dir, kept) != 0) {
        remove_dir(fresh);
        return WIISAVE_UNWRITABLE;
    }
    if (rename(fresh, dir) != 0) {
        rename(kept, dir);
        remove_dir(fresh);
        return WIISAVE_UNWRITABLE;
    }
    return WIISAVE_OK;
}

/* ------------------------------------------------------------------------ */
/* A zip of the files (Android's picker can't choose a folder) */

/* Raw DEFLATE (RFC 1951), as puff.c does it: in[0..n) into out, exactly
 * want bytes; 0 if it isn't that. */
typedef struct {
    const uint8_t* in;
    size_t n, pos;
    uint32_t bits, have;
    uint8_t* out;
    size_t len, want;
    int bad;
} Inflate;

typedef struct {
    short count[16], sym[288];
} Huffman;

static uint32_t take_bits(Inflate* s, int need) {
    uint32_t v = s->bits;
    while (s->have < (uint32_t)need) {
        if (s->pos >= s->n) {
            s->bad = 1;
            return 0;
        }
        v |= (uint32_t)s->in[s->pos++] << s->have;
        s->have += 8;
    }
    s->bits = v >> need;
    s->have -= (uint32_t)need;
    return v & ((1u << need) - 1);
}

static int decode(Inflate* s, const Huffman* h) {
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        code |= (int)take_bits(s, 1);
        if (s->bad) return -1;
        if (code - h->count[len] < first) return h->sym[index + (code - first)];
        index += h->count[len];
        first = (first + h->count[len]) << 1;
        code <<= 1;
    }
    return -1;
}

/* 0 if the lengths make a code that's over-full. */
static int build(Huffman* h, const short* length, int n) {
    short offs[16];
    int i, left = 1;
    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < n; i++) h->count[length[i]]++;
    for (i = 1; i < 16; i++) {
        left = (left << 1) - h->count[i];
        if (left < 0) return 0;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (length[i]) h->sym[offs[length[i]]++] = (short)i;
    return 1;
}

static int codes(Inflate* s, const Huffman* lit, const Huffman* dist) {
    static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0 || s->bad) return 0;
        if (sym < 256) {
            if (s->len >= s->want) return 0;
            s->out[s->len++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            size_t len, back;
            sym -= 257;
            if (sym >= 29) return 0;
            len = (size_t)lbase[sym] + take_bits(s, lext[sym]);
            sym = decode(s, dist);
            if (sym < 0 || sym >= 30) return 0;
            back = (size_t)dbase[sym] + take_bits(s, dext[sym]);
            if (s->bad || back > s->len || len > s->want - s->len) return 0;
            for (; len; len--, s->len++) s->out[s->len] = s->out[s->len - back];
        }
    }
}

static int inflate_raw(const uint8_t* in, size_t n, uint8_t* out, size_t want) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    Inflate s;
    Huffman lit, dist;
    short length[320];
    int last, type, i;
    memset(&s, 0, sizeof(s));
    s.in = in;
    s.n = n;
    s.out = out;
    s.want = want;
    do {
        last = (int)take_bits(&s, 1);
        type = (int)take_bits(&s, 2);
        if (s.bad) return 0;
        if (type == 0) { /* stored */
            uint32_t len;
            s.bits = s.have = 0;
            if (s.pos + 4 > n) return 0;
            len = in[s.pos] | (uint32_t)in[s.pos + 1] << 8;
            if ((len ^ 0xFFFFu) != (in[s.pos + 2] | (uint32_t)in[s.pos + 3] << 8)) return 0;
            s.pos += 4;
            if (len > n - s.pos || len > want - s.len) return 0;
            memcpy(out + s.len, in + s.pos, len);
            s.pos += len;
            s.len += len;
        } else if (type == 1) { /* fixed codes */
            for (i = 0; i < 288; i++) length[i] = (short)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
            build(&lit, length, 288);
            for (i = 0; i < 30; i++) length[i] = 5;
            build(&dist, length, 30);
            if (!codes(&s, &lit, &dist)) return 0;
        } else if (type == 2) { /* dynamic codes */
            int nlen = (int)take_bits(&s, 5) + 257, ndist = (int)take_bits(&s, 5) + 1, ncode = (int)take_bits(&s, 4) + 4;
            Huffman lencode;
            if (s.bad || nlen > 286 || ndist > 30) return 0;
            memset(length, 0, sizeof(length));
            for (i = 0; i < ncode; i++) length[order[i]] = (short)take_bits(&s, 3);
            if (s.bad || !build(&lencode, length, 19)) return 0;
            for (i = 0; i < nlen + ndist;) {
                int sym = decode(&s, &lencode), rep;
                short len = 0;
                if (sym < 0) return 0;
                if (sym < 16) {
                    length[i++] = (short)sym;
                    continue;
                }
                if (sym == 16) {
                    if (i == 0) return 0;
                    len = length[i - 1];
                    rep = 3 + (int)take_bits(&s, 2);
                } else if (sym == 17) rep = 3 + (int)take_bits(&s, 3);
                else rep = 11 + (int)take_bits(&s, 7);
                if (s.bad || i + rep > nlen + ndist) return 0;
                while (rep--) length[i++] = len;
            }
            if (!length[256] || !build(&lit, length, nlen) || !build(&dist, length + nlen, ndist)) return 0;
            if (!codes(&s, &lit, &dist)) return 0;
        } else {
            return 0;
        }
    } while (!last);
    return s.len == want;
}

static uint32_t le16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t* p) { return le16(p) | le16(p + 2) << 16; }

int wiisave_is_zip(const uint8_t* data, size_t size) {
    return size >= 22 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4;
}

int wiisave_read_zip(const uint8_t* zip, size_t size, WiiSaveFile* files, int max, int* count) {
    size_t end, at;
    uint32_t entries, i;
    *count = 0;
    if (!wiisave_is_zip(zip, size)) return WIISAVE_NOT_A_SAVE;
    /* The end of the central directory: last, after a comment of up to 64K. */
    for (end = size - 22;; end--) {
        if (le32(zip + end) == 0x06054B50u) break;
        if (end == 0 || size - end > 22 + 0xFFFF) return WIISAVE_NOT_A_SAVE;
    }
    entries = le16(zip + end + 10);
    at = le32(zip + end + 16);
    for (i = 0; i < entries; i++) {
        const uint8_t* e = zip + at;
        uint32_t method, csize, usize, nlen, local, data;
        const char *name, *base;
        char full[256];
        uint8_t* out;
        if (at + 46 > end || le32(e) != 0x02014B50u) return WIISAVE_NOT_A_SAVE;
        method = le16(e + 10);
        csize = le32(e + 20);
        usize = le32(e + 24);
        nlen = le16(e + 28);
        local = le32(e + 42);
        if (at + 46 + nlen > end) return WIISAVE_NOT_A_SAVE;
        snprintf(full, sizeof(full), "%.*s", (int)(nlen < 255 ? nlen : 255), (const char*)e + 46);
        at += 46 + nlen + le16(e + 30) + le16(e + 32);
        /* Folders, and what macOS adds (__MACOSX/, ._ files), passed by;
         * the files are taken by their names, whatever folder they're in. */
        name = full;
        base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if (!*base || base[0] == '.' || strstr(name, "__MACOSX")) continue;
        if (!wiisave_own_name(base)) continue;
        if (*count >= max || usize > (16u << 20)) goto bad;
        if ((size_t)local + 30 > size || le32(zip + local) != 0x04034B50u) goto bad;
        data = local + 30 + le16(zip + local + 26) + le16(zip + local + 28);
        if (data > size || csize > size - data) goto bad;
        if (!(out = (uint8_t*)malloc(usize ? usize : 1))) goto bad;
        if (method == 0 && csize == usize) memcpy(out, zip + data, usize);
        else if (method != 8 || !inflate_raw(zip + data, csize, out, usize)) {
            free(out);
            goto bad;
        }
        snprintf(files[*count].name, sizeof(files[*count].name), "%s", base);
        files[*count].data = out;
        files[*count].size = usize;
        ++*count;
    }
    if (wiisave_check_files(files, *count)) return WIISAVE_OK;
    while (*count) free((void*)files[--*count].data);
    return WIISAVE_NOT_A_SAVE;
bad:
    while (*count) free((void*)files[--*count].data);
    return WIISAVE_UNREADABLE;
}

int wiisave_import_files(const char* nand_root, WiiSaveFile* files, int count) {
    int n, r;
    if (count == 1 && wiisave_is_zip(files[0].data, files[0].size)) {
        WiiSaveFile in_zip[40];
        r = wiisave_read_zip(files[0].data, files[0].size, in_zip, 40, &n);
        if (r != WIISAVE_OK) return r;
        r = wiisave_import_files(nand_root, in_zip, n);
        while (n > 0) free((void*)in_zip[--n].data);
        return r;
    }
    if (!wiisave_check_files(files, count)) return WIISAVE_NOT_A_SAVE;
    return wiisave_install(nand_root, files, count);
}

/* PokePark 2's: GD0 to GD2 (a save slot each) and GD0M to GD2M (their
 * copies), Pic0 to Pic2 (each slot's photos) and banner.bin. */
int wiisave_own_name(const char* s) {
    size_t n = strlen(s);
    if (!strcmp(s, "banner.bin")) return 1;
    if (n == 4 && s[0] == 'G' && s[1] == 'D' && s[2] >= '0' && s[2] <= '9' && s[3] == 'M') return 1;
    if (n < 3 || s[n - 1] < '0' || s[n - 1] > '9') return 0;
    return (n == 3 && s[0] == 'G' && s[1] == 'D') || (n == 4 && !strncmp(s, "Pic", 3));
}

/* ------------------------------------------------------------------------ */
/* Taking it out: a zip of the save's files, as in Dolphin's folder */

const char* wiisave_export_message(int result) {
    switch (result) {
    case WIISAVE_OK: return "Exported";
    case WIISAVE_NOT_A_SAVE: return "No save yet";
    case WIISAVE_UNREADABLE: return "Couldn't read it";
    default: return "Couldn't write it";
    }
}

static uint32_t crc32_of(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (!table[1]) {
        for (i = 0; i < 256; i++) {
            uint32_t v = (uint32_t)i;
            int k;
            for (k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
    }
    for (i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint8_t* put16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t* put32(uint8_t* p, uint32_t v) { return put16(put16(p, v & 0xFFFF), v >> 16); }

/* Deflate (RFC 1951), simply: one block of the fixed codes, each match the
 * last place its first three bytes were seen. A save's files are mostly
 * zeros (the photo slots, unused), 12 MB that come to a few KB. */
typedef struct {
    uint8_t* out;
    size_t at, cap;
    uint32_t bits;
    int count;
} Bits;

static void put_bits(Bits* b, uint32_t v, int n) {
    b->bits |= v << b->count;
    b->count += n;
    while (b->count >= 8) {
        if (b->at < b->cap) b->out[b->at] = (uint8_t)b->bits;
        b->at++;
        b->bits >>= 8;
        b->count -= 8;
    }
}

/* A Huffman code, n bits, written first bit first. */
static void put_code(Bits* b, uint32_t code, int n) {
    uint32_t r = 0;
    int i;
    for (i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(b, r, n);
}

static void put_symbol(Bits* b, int s) {
    if (s < 144) put_code(b, 0x30 + s, 8);
    else if (s < 256) put_code(b, 0x190 + s - 144, 9);
    else if (s < 280) put_code(b, s - 256, 7);
    else put_code(b, 0xC0 + s - 280, 8);
}

static const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

#define HASH_BITS 15

/* in, n bytes, deflated into out (cap bytes): its size, or 0 if it didn't
 * fit (then it's stored instead). */
static size_t deflate_fixed(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
    Bits b = {out, 0, cap, 0, 0};
    int32_t* last = (int32_t*)malloc(sizeof(int32_t) << HASH_BITS);
    size_t i = 0;
    if (!last) return 0;
    memset(last, 0xFF, sizeof(int32_t) << HASH_BITS);
    put_bits(&b, 1, 1); /* the last block */
    put_bits(&b, 1, 2); /* fixed codes */
    while (i < n && b.at < cap) {
        size_t len = 0, dist = 0;
        if (i + 3 <= n) {
            uint32_t h = ((uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2]) * 2654435761u >> (32 - HASH_BITS);
            int32_t at = last[h];
            last[h] = (int32_t)i;
            if (at >= 0 && i - (size_t)at <= 32768) {
                size_t max = n - i < 258 ? n - i : 258;
                while (len < max && in[at + len] == in[i + len]) len++;
                dist = i - (size_t)at;
            }
        }
        if (len >= 3) {
            int c = 28, d = 29;
            while (kLenBase[c] > len) c--;
            while (kDistBase[d] > dist) d--;
            put_symbol(&b, 257 + c);
            put_bits(&b, (uint32_t)(len - kLenBase[c]), kLenExtra[c]);
            put_code(&b, (uint32_t)d, 5);
            put_bits(&b, (uint32_t)(dist - kDistBase[d]), kDistExtra[d]);
            i += len;
        } else {
            put_symbol(&b, in[i++]);
        }
    }
    put_symbol(&b, 256);
    put_bits(&b, 0, 7); /* to a whole byte */
    free(last);
    return b.at <= cap ? b.at : 0;
}

uint8_t* wiisave_write_zip(const WiiSaveFile* files, int count, uint32_t dos_time, size_t* size) {
    size_t total = 22, at = 0, dir;
    uint8_t *zip = NULL, *p, **packed;
    uint32_t *crc, *csize;
    int i;
    packed = (uint8_t**)calloc(count ? count : 1, sizeof(uint8_t*));
    crc = (uint32_t*)malloc(sizeof(uint32_t) * (count ? count : 1));
    csize = (uint32_t*)malloc(sizeof(uint32_t) * (count ? count : 1));
    if (!packed || !crc || !csize) goto done;
    /* Each file deflated, or stored where that's no smaller. */
    for (i = 0; i < count; i++) {
        crc[i] = crc32_of(files[i].data, files[i].size);
        csize[i] = files[i].size;
        if (files[i].size && (packed[i] = (uint8_t*)malloc(files[i].size))) {
            size_t n = deflate_fixed(files[i].data, files[i].size, packed[i], files[i].size - 1);
            if (n) csize[i] = (uint32_t)n;
            else free(packed[i]), packed[i] = NULL;
        }
        total += 30 + 46 + 2 * strlen(files[i].name) + csize[i];
    }
    if (!(zip = (uint8_t*)malloc(total))) goto done;
    p = zip;
    for (i = 0; i < count; i++) {
        size_t n = strlen(files[i].name);
        p = put32(p, 0x04034B50u);
        p = put16(p, 20);                /* version needed: 2.0 */
        p = put16(p, 0);                 /* flags */
        p = put16(p, packed[i] ? 8 : 0); /* deflated or stored */
        p = put32(p, dos_time);
        p = put32(p, crc[i]);
        p = put32(p, csize[i]);
        p = put32(p, files[i].size);
        p = put16(p, (uint32_t)n);
        p = put16(p, 0);
        memcpy(p, files[i].name, n);
        p += n;
        memcpy(p, packed[i] ? packed[i] : files[i].data, csize[i]);
        p += csize[i];
    }
    dir = (size_t)(p - zip);
    for (i = 0; i < count; i++) {
        size_t n = strlen(files[i].name);
        p = put32(p, 0x02014B50u);
        p = put16(p, 20); /* made by: MS-DOS, 2.0 */
        p = put16(p, 20);
        p = put16(p, 0);
        p = put16(p, packed[i] ? 8 : 0);
        p = put32(p, dos_time);
        p = put32(p, crc[i]);
        p = put32(p, csize[i]);
        p = put32(p, files[i].size);
        p = put16(p, (uint32_t)n);
        p = put16(p, 0); /* extra */
        p = put16(p, 0); /* comment */
        p = put16(p, 0); /* disk */
        p = put16(p, 0); /* internal attributes */
        p = put32(p, 0); /* external attributes */
        p = put32(p, (uint32_t)at);
        memcpy(p, files[i].name, n);
        p += n;
        at += 30 + n + csize[i];
    }
    p = put32(p, 0x06054B50u);
    p = put16(p, 0);
    p = put16(p, 0);
    p = put16(p, (uint32_t)count);
    p = put16(p, (uint32_t)count);
    p = put32(p, (uint32_t)((size_t)(p - zip) - dir - 12));
    p = put32(p, (uint32_t)dir);
    p = put16(p, 0);
    *size = (size_t)(p - zip);
done:
    for (i = 0; packed && i < count; i++) free(packed[i]);
    free(packed);
    free(crc);
    free(csize);
    return zip;
}

uint32_t wiisave_dos_now(void) {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    if (!t || t->tm_year < 80) return 0x00210000u; /* 1980-01-01 */
    return (uint32_t)(t->tm_year - 80) << 25 | (uint32_t)(t->tm_mon + 1) << 21 | (uint32_t)t->tm_mday << 16 |
           (uint32_t)t->tm_hour << 11 | (uint32_t)t->tm_min << 5 | (uint32_t)(t->tm_sec / 2);
}

/* The file at path, read into f as name; 0 if it couldn't be. */
static int read_one(const char* path, const char* name, WiiSaveFile* f) {
    FILE* in = fopen(path, "rb");
    long size;
    uint8_t* data;
    if (!in) return 0;
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (size < 0 || size > (16L << 20) || !(data = (uint8_t*)malloc(size ? (size_t)size : 1))) {
        fclose(in);
        return 0;
    }
    if (fread(data, 1, (size_t)size, in) != (size_t)size) {
        free(data);
        fclose(in);
        return 0;
    }
    fclose(in);
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->data = data;
    f->size = (uint32_t)size;
    return 1;
}

int wiisave_export_zip(const char* nand_root, uint32_t dos_time, uint8_t** zip, size_t* size) {
    char dir[1100], path[1200];
    WiiSaveFile files[32];
    int count = 0, r = WIISAVE_OK;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
#else
    DIR* d;
    struct dirent* e;
#endif
    *zip = NULL;
    *size = 0;
    snprintf(dir, sizeof(dir), "%s/title/00010000/53324c45/data", nand_root);
#ifdef _WIN32
    snprintf(path, sizeof(path), "%s/*", dir);
    if ((h = FindFirstFileA(path, &fd)) == INVALID_HANDLE_VALUE) return WIISAVE_NOT_A_SAVE;
    do {
        const char* name = fd.cFileName;
#else
    if (!(d = opendir(dir))) return WIISAVE_NOT_A_SAVE;
    while ((e = readdir(d)) != NULL) {
        const char* name = e->d_name;
#endif
        if (!wiisave_own_name(name)) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (count == 32 || !read_one(path, name, &files[count])) {
            r = WIISAVE_UNREADABLE;
            break;
        }
        count++;
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    if (r == WIISAVE_OK && !wiisave_check_files(files, count)) r = WIISAVE_NOT_A_SAVE;
    if (r == WIISAVE_OK && !(*zip = wiisave_write_zip(files, count, dos_time, size))) r = WIISAVE_UNREADABLE;
    while (count > 0) free((void*)files[--count].data);
    return r;
}
