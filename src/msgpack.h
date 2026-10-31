/**
 * msgpack as msgpackr 1.11.2 writes it, which is what swim-ng's codec binds.
 *
 * Plain C, read as it is by meta and by the C compiler; the implementation
 * is msgpack.c. Three layers:
 *
 *   mp_buf_t      a growing byte buffer and the writer. Every put writes the
 *                 shape msgpackr's default Packr({useRecords: false}) would
 *                 write for the same JS value - a map is always a map16, a
 *                 number that is not an int32 or a uint32 is a float64.
 *   mp_reader_t   a cursor over bytes: what is next, read it, skip it. What
 *                 swim.c uses, because a member's meta is passed along as the
 *                 bytes it came in and never has to become a tree.
 *   json_t        a whole value as meta's document type (yyjson underneath),
 *                 for what a program wants to look into: mp_decode, mp_encode.
 *
 * Why a number is written the JS way: on the other side a uint64 comes out of
 * msgpackr as a BigInt, and swim-ng does `incarnation + 1` with it - a
 * TypeError. Date.now() goes out as a float64 (0xcb) because it is neither an
 * int32 nor a uint32, so an incarnation has to go back the same way.
 * docs/protocol.md has the whole table.
 *
 * The typed dump (mp_dump, mp_dumpTree) is the text test/harness/lib/mpdump.js
 * writes for the same bytes, so a golden file says what the bytes *were*:
 *
 *   nil true false u:5 i:-5 f:1730000000000 f:0x3ff8000000000000
 *   "text" bin:00ff [a,b] {"k":v}
 */
#ifndef MWM_MSGPACK_H
#define MWM_MSGPACK_H

#include <stddef.h>
#include <stdint.h>

#include <meta_json.h>
#include <meta_owns.h>

/* ------------------------------------------------------------------ writer */

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;

  /** Set once an allocation failed; every put after that is a no-op. */
  int failed;
} mp_buf_t;

void mp_buf_t__release(mp_buf_t *self);
void mp_buf_t__reset(mp_buf_t *self);
void mp_buf_t__raw(mp_buf_t *self, const void *bytes, size_t size);
void mp_buf_t__byte(mp_buf_t *self, uint8_t byte);
void mp_buf_t__u16le(mp_buf_t *self, uint16_t value);
void mp_buf_t__text(mp_buf_t *self, const char *text);

void mp_buf_t__nil(mp_buf_t *self);
void mp_buf_t__boolean(mp_buf_t *self, int value);

/** A JS number: int32/uint32 as the smallest int form, anything else float64. */
void mp_buf_t__number(mp_buf_t *self, double value);

/** The same rule from an integer: outside int32/uint32 it becomes a float64. */
void mp_buf_t__integer(mp_buf_t *self, int64_t value);

/** Always 0xcb, whatever the value - for a field the other side keeps as a double. */
void mp_buf_t__float64(mp_buf_t *self, double value);

void mp_buf_t__str(mp_buf_t *self, const char *text, size_t size);
void mp_buf_t__cstr(mp_buf_t *self, const char *text);
void mp_buf_t__bin(mp_buf_t *self, const void *bytes, size_t size);
void mp_buf_t__array(mp_buf_t *self, uint32_t count);

/** map16 as msgpackr writes it for every object; map32 past 65535 entries. */
void mp_buf_t__map(mp_buf_t *self, uint32_t count);

/* ------------------------------------------------------------------ reader */

typedef enum {
  MP_END = 0,
  MP_NIL,
  MP_BOOL,
  MP_UINT,
  MP_INT,
  MP_FLOAT,
  MP_STR,
  MP_BIN,
  MP_ARRAY,
  MP_MAP,
  MP_EXT,
  MP_BAD
} mp_kind_t;

typedef struct {
  const uint8_t *data;
  size_t len;
  size_t at;

  /** What went wrong first; NULL while all is well. */
  const char *error;
} mp_reader_t;

mp_reader_t mp_reader(const void *data, size_t len);

/** What the next value is, without taking it. MP_END at the end. */
mp_kind_t mp_reader_t__peek(mp_reader_t *self);

int mp_reader_t__nil(mp_reader_t *self);
int mp_reader_t__boolean(mp_reader_t *self, int *into);

/** Any numeric form, as a double. */
int mp_reader_t__number(mp_reader_t *self, double *into);

/** Any numeric form holding a whole number that fits, as an int64. */
int mp_reader_t__integer(mp_reader_t *self, int64_t *into);

/** A str; `*text` points into the input and is not terminated. */
int mp_reader_t__str(mp_reader_t *self, const char **text, uint32_t *size);

/** A str compared with a C string, taken when it matches. */
int mp_reader_t__key(mp_reader_t *self, const char *wanted);

int mp_reader_t__bin(mp_reader_t *self, const uint8_t **bytes, uint32_t *size);
int mp_reader_t__array(mp_reader_t *self, uint32_t *count);
int mp_reader_t__map(mp_reader_t *self, uint32_t *count);

/** Steps over one whole value, containers included. */
int mp_reader_t__skip(mp_reader_t *self);

/* -------------------------------------------------------------------- tree */

/**
 * One whole value as a document, owned by the caller - release it.
 *
 * uint and positive fixint become yyjson uints, the int forms sints, both
 * floats reals; bin becomes a raw node (yyjson's RAW, which nothing else
 * here produces) so mp_encode can write it back as bin; msgpackr's undefined
 * (fixext 1, type 0) becomes null. Any other ext is refused. Trailing bytes
 * are refused too: a body is exactly one value.
 */
META_OWNS json_t mp_decode(const void *data, size_t len);

/** Writes a document the way msgpackr writes the JS value it would parse to. */
int mp_encode(mp_buf_t *into, json_t value);

/* -------------------------------------------------------------- typed dump */

/** The typed dump of one value read from the bytes; 0 if they were not one. */
int mp_dump(const void *data, size_t len, mp_buf_t *text);

/** The same dump from a decoded document. */
int mp_dumpTree(json_t value, mp_buf_t *text);

#endif
