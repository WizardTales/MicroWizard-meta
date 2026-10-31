/**
 * msgpack, in the shapes msgpackr 1.11.2 writes. msgpack.h says what and why;
 * this is the how. The rule that matters is in `mp_buf_t.number`.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msgpack.h"

/* ------------------------------------------------------------------ buffer */

static int grow(mp_buf_t *self, size_t more) {

  size_t want;
  uint8_t *bigger;

  if (self->failed)
    return 0;

  if (self->len + more <= self->cap)
    return 1;

  want = self->cap != 0 ? self->cap : 64;

  while (want < self->len + more)
    want *= 2;

  bigger = (uint8_t *)realloc(self->data, want);

  if (bigger == NULL) {
    self->failed = 1;
    return 0;
  }

  self->data = bigger;
  self->cap = want;

  return 1;
}

void mp_buf_t.release(mp_buf_t *self) {

  free(self->data);
  self->data = NULL;
  self->len = 0;
  self->cap = 0;
  self->failed = 0;
}

void mp_buf_t.reset(mp_buf_t *self) {

  self->len = 0;
  self->failed = 0;
}

void mp_buf_t.raw(mp_buf_t *self, const void *bytes, size_t size) {

  if (size == 0 || !grow(self, size))
    return;

  memcpy(self->data + self->len, bytes, size);
  self->len += size;
}

void mp_buf_t.byte(mp_buf_t *self, uint8_t byte) {

  if (!grow(self, 1))
    return;

  self->data[self->len++] = byte;
}

void mp_buf_t.u16le(mp_buf_t *self, uint16_t value) {

  self->byte((uint8_t)(value & 0xff));
  self->byte((uint8_t)(value >> 8));
}

void mp_buf_t.text(mp_buf_t *self, const char *text) {
  self->raw(text, strlen(text));
}

static void be16(mp_buf_t *self, uint8_t head, uint16_t value) {

  uint8_t out[3] = {head, (uint8_t)(value >> 8), (uint8_t)value};

  self->raw(out, sizeof out);
}

static void be32(mp_buf_t *self, uint8_t head, uint32_t value) {

  uint8_t out[5] = {head, (uint8_t)(value >> 24), (uint8_t)(value >> 16),
                    (uint8_t)(value >> 8), (uint8_t)value};

  self->raw(out, sizeof out);
}

static void be64(mp_buf_t *self, uint8_t head, uint64_t value) {

  uint8_t out[9];

  out[0] = head;

  for (int i = 0; i < 8; ++i)
    out[1 + i] = (uint8_t)(value >> (56 - 8 * i));

  self->raw(out, sizeof out);
}

/* ------------------------------------------------------------------ writer */

void mp_buf_t.nil(mp_buf_t *self) { self->byte(0xc0); }

void mp_buf_t.boolean(mp_buf_t *self, int value) {
  self->byte(value ? 0xc3 : 0xc2);
}

void mp_buf_t.float64(mp_buf_t *self, double value) {

  uint64_t bits;

  memcpy(&bits, &value, sizeof bits);
  be64(self, 0xcb, bits);
}

/**
 * msgpackr's pack for a JS number, branch for branch:
 *
 *   value >>> 0 === value   uint32: fixint below 0x80 (useRecords is false),
 *                           else 0xcc, 0xcd, 0xce
 *   value >> 0 === value    int32: negative fixint from -32, else 0xd0,
 *                           0xd1, 0xd2
 *   anything else           0xcb (useFloat32 is not set)
 *
 * -0 passes the first test (-0 >>> 0 is 0 and 0 === -0), so it is a fixint 0.
 * NaN and the infinities fail both and are float64s.
 */
void mp_buf_t.number(mp_buf_t *self, double value) {

  if (value >= 0 && value <= 4294967295.0 && value == floor(value)) {

    uint32_t u = (uint32_t)value;

    if (u < 0x80)
      self->byte((uint8_t)u);
    else if (u < 0x100) {
      uint8_t out[2] = {0xcc, (uint8_t)u};
      self->raw(out, 2);
    } else if (u < 0x10000)
      be16(self, 0xcd, (uint16_t)u);
    else
      be32(self, 0xce, u);

    return;
  }

  if (value < 0 && value >= -2147483648.0 && value == floor(value)) {

    int32_t i = (int32_t)value;

    if (i >= -32)
      self->byte((uint8_t)(0x100 + i));
    else if (i >= -128) {
      uint8_t out[2] = {0xd0, (uint8_t)(i + 0x100)};
      self->raw(out, 2);
    } else if (i >= -32768)
      be16(self, 0xd1, (uint16_t)(int16_t)i);
    else
      be32(self, 0xd2, (uint32_t)i);

    return;
  }

  self->float64(value);
}

void mp_buf_t.integer(mp_buf_t *self, int64_t value) {

  /* in range, the double is exact and number() picks the int form */
  self->number((double)value);
}

static void header(mp_buf_t *self, size_t size, uint8_t fix, uint8_t fixMax,
                   uint8_t b8, uint8_t b16, uint8_t b32) {

  if (fix != 0 && size < fixMax)
    self->byte((uint8_t)(fix | size));
  else if (b8 != 0 && size < 0x100) {
    uint8_t out[2] = {b8, (uint8_t)size};
    self->raw(out, 2);
  } else if (size < 0x10000)
    be16(self, b16, (uint16_t)size);
  else
    be32(self, b32, (uint32_t)size);
}

void mp_buf_t.str(mp_buf_t *self, const char *text, size_t size) {

  header(self, size, 0xa0, 0x20, 0xd9, 0xda, 0xdb);
  self->raw(text, size);
}

void mp_buf_t.cstr(mp_buf_t *self, const char *text) {
  self->str(text, strlen(text));
}

void mp_buf_t.bin(mp_buf_t *self, const void *bytes, size_t size) {

  header(self, size, 0, 0, 0xc4, 0xc5, 0xc6);
  self->raw(bytes, size);
}

void mp_buf_t.array(mp_buf_t *self, uint32_t count) {
  header(self, count, 0x90, 0x10, 0, 0xdc, 0xdd);
}

void mp_buf_t.map(mp_buf_t *self, uint32_t count) {

  /* msgpackr reserves the 16 bit size before it knows it; it never writes a fixmap */
  if (count < 0x10000)
    be16(self, 0xde, (uint16_t)count);
  else
    be32(self, 0xdf, count);
}

/* ------------------------------------------------------------------ reader */

mp_reader_t mp_reader(const void *data, size_t len) {

  mp_reader_t r;

  r.data = (const uint8_t *)data;
  r.len = len;
  r.at = 0;
  r.error = NULL;

  return r;
}

static int fail(mp_reader_t *self, const char *why) {

  if (self->error == NULL)
    self->error = why;

  return 0;
}

static int need(mp_reader_t *self, size_t count) {

  if (self->error != NULL)
    return 0;

  if (self->len - self->at < count)
    return fail(self, "truncated");

  return 1;
}

static uint64_t bigEndian(const uint8_t *at, int count) {

  uint64_t value = 0;

  for (int i = 0; i < count; ++i)
    value = value << 8 | at[i];

  return value;
}

mp_kind_t mp_reader_t.peek(mp_reader_t *self) {

  uint8_t b;

  if (self->error != NULL)
    return MP_BAD;

  if (self->at >= self->len)
    return MP_END;

  b = self->data[self->at];

  if (b <= 0x7f)
    return MP_UINT;
  if (b >= 0xe0)
    return MP_INT;
  if ((b & 0xf0) == 0x80)
    return MP_MAP;
  if ((b & 0xf0) == 0x90)
    return MP_ARRAY;
  if ((b & 0xe0) == 0xa0)
    return MP_STR;

  switch (b) {
  case 0xc0:
    return MP_NIL;
  case 0xc2:
  case 0xc3:
    return MP_BOOL;
  case 0xc4:
  case 0xc5:
  case 0xc6:
    return MP_BIN;
  case 0xc7:
  case 0xc8:
  case 0xc9:
  case 0xd4:
  case 0xd5:
  case 0xd6:
  case 0xd7:
  case 0xd8:
    return MP_EXT;
  case 0xca:
  case 0xcb:
    return MP_FLOAT;
  case 0xcc:
  case 0xcd:
  case 0xce:
  case 0xcf:
    return MP_UINT;
  case 0xd0:
  case 0xd1:
  case 0xd2:
  case 0xd3:
    return MP_INT;
  case 0xd9:
  case 0xda:
  case 0xdb:
    return MP_STR;
  case 0xdc:
  case 0xdd:
    return MP_ARRAY;
  case 0xde:
  case 0xdf:
    return MP_MAP;
  default:
    return MP_BAD;
  }
}

int mp_reader_t.nil(mp_reader_t *self) {

  if (!need(self, 1))
    return 0;

  if (self->data[self->at] != 0xc0)
    return fail(self, "not nil");

  self->at += 1;

  return 1;
}

int mp_reader_t.boolean(mp_reader_t *self, int *into) {

  uint8_t b;

  if (!need(self, 1))
    return 0;

  b = self->data[self->at];

  if (b != 0xc2 && b != 0xc3)
    return fail(self, "not a boolean");

  *into = b == 0xc3;
  self->at += 1;

  return 1;
}

/**
 * Any numeric form. Kept apart as what it was - unsigned, signed or a double -
 * so neither number() nor integer() has to guess from a double whether a
 * uint64 was exact.
 */
static int numeric(mp_reader_t *self, int *kind, uint64_t *u, int64_t *s,
                   double *d) {

  uint8_t b;
  const uint8_t *at;

  if (!need(self, 1))
    return 0;

  b = self->data[self->at];
  at = self->data + self->at + 1;

  if (b <= 0x7f) {
    *kind = 'u';
    *u = b;
    self->at += 1;
    return 1;
  }

  if (b >= 0xe0) {
    *kind = 's';
    *s = (int8_t)b;
    self->at += 1;
    return 1;
  }

  switch (b) {
  case 0xcc:
  case 0xcd:
  case 0xce:
  case 0xcf: {
    int width = 1 << (b - 0xcc);

    if (!need(self, 1 + (size_t)width))
      return 0;

    *kind = 'u';
    *u = bigEndian(at, width);
    self->at += 1 + (size_t)width;
    return 1;
  }
  case 0xd0:
  case 0xd1:
  case 0xd2:
  case 0xd3: {
    int width = 1 << (b - 0xd0);
    uint64_t raw;

    if (!need(self, 1 + (size_t)width))
      return 0;

    raw = bigEndian(at, width);

    /* sign-extend from the width it was written in */
    if (width < 8 && (raw >> (width * 8 - 1)) != 0)
      raw |= ~(uint64_t)0 << (width * 8);

    *kind = 's';
    *s = (int64_t)raw;
    self->at += 1 + (size_t)width;
    return 1;
  }
  case 0xca: {
    uint32_t bits;
    float f;

    if (!need(self, 5))
      return 0;

    bits = (uint32_t)bigEndian(at, 4);
    memcpy(&f, &bits, sizeof f);
    *kind = 'd';
    *d = f;
    self->at += 5;
    return 1;
  }
  case 0xcb: {
    uint64_t bits;

    if (!need(self, 9))
      return 0;

    bits = bigEndian(at, 8);
    memcpy(d, &bits, sizeof *d);
    *kind = 'd';
    self->at += 9;
    return 1;
  }
  default:
    return fail(self, "not a number");
  }
}

int mp_reader_t.number(mp_reader_t *self, double *into) {

  int kind;
  uint64_t u;
  int64_t s;
  double d;

  if (!numeric(self, &kind, &u, &s, &d))
    return 0;

  *into = kind == 'u' ? (double)u : kind == 's' ? (double)s : d;

  return 1;
}

int mp_reader_t.integer(mp_reader_t *self, int64_t *into) {

  int kind;
  uint64_t u;
  int64_t s;
  double d;
  size_t was = self->at;

  if (!numeric(self, &kind, &u, &s, &d))
    return 0;

  if (kind == 'u') {

    if (u > (uint64_t)INT64_MAX) {
      self->at = was;
      return fail(self, "integer out of range");
    }

    *into = (int64_t)u;
    return 1;
  }

  if (kind == 's') {
    *into = s;
    return 1;
  }

  /* a JS number: Date.now() arrives here, as a double holding a whole number */
  if (d != floor(d) || d < -9223372036854775808.0 || d >= 9223372036854775808.0) {
    self->at = was;
    return fail(self, "not a whole number");
  }

  *into = (int64_t)d;

  return 1;
}

static int lengthOf(mp_reader_t *self, uint8_t fixMask, uint8_t fixTag,
                    uint8_t fixBits, uint8_t b8, uint8_t b16, uint8_t b32,
                    uint32_t *size, const char *what) {

  uint8_t b;
  const uint8_t *at;

  if (!need(self, 1))
    return 0;

  b = self->data[self->at];
  at = self->data + self->at + 1;

  if (fixMask != 0 && (b & fixMask) == fixTag) {
    *size = b & fixBits;
    self->at += 1;
    return 1;
  }

  if (b8 != 0 && b == b8) {
    if (!need(self, 2))
      return 0;
    *size = at[0];
    self->at += 2;
    return 1;
  }

  if (b == b16) {
    if (!need(self, 3))
      return 0;
    *size = (uint32_t)bigEndian(at, 2);
    self->at += 3;
    return 1;
  }

  if (b == b32) {
    if (!need(self, 5))
      return 0;
    *size = (uint32_t)bigEndian(at, 4);
    self->at += 5;
    return 1;
  }

  return fail(self, what);
}

int mp_reader_t.str(mp_reader_t *self, const char **text, uint32_t *size) {

  if (!lengthOf(self, 0xe0, 0xa0, 0x1f, 0xd9, 0xda, 0xdb, size, "not a str"))
    return 0;

  if (!need(self, *size))
    return 0;

  *text = (const char *)self->data + self->at;
  self->at += *size;

  return 1;
}

int mp_reader_t.key(mp_reader_t *self, const char *wanted) {

  mp_reader_t probe = *self;
  const char *text;
  uint32_t size;

  if (probe.peek() != MP_STR || !probe.str(&text, &size))
    return 0;

  if (size != strlen(wanted) || memcmp(text, wanted, size) != 0)
    return 0;

  *self = probe;

  return 1;
}

int mp_reader_t.bin(mp_reader_t *self, const uint8_t **bytes, uint32_t *size) {

  if (!lengthOf(self, 0, 0, 0, 0xc4, 0xc5, 0xc6, size, "not a bin"))
    return 0;

  if (!need(self, *size))
    return 0;

  *bytes = self->data + self->at;
  self->at += *size;

  return 1;
}

int mp_reader_t.array(mp_reader_t *self, uint32_t *count) {
  return lengthOf(self, 0xf0, 0x90, 0x0f, 0, 0xdc, 0xdd, count,
                  "not an array");
}

int mp_reader_t.map(mp_reader_t *self, uint32_t *count) {
  return lengthOf(self, 0xf0, 0x80, 0x0f, 0, 0xde, 0xdf, count, "not a map");
}

/** An ext of any size: the type byte, then the data. */
static int extension(mp_reader_t *self, int8_t *type, const uint8_t **data,
                     uint32_t *size) {

  uint8_t b;

  if (!need(self, 2))
    return 0;

  b = self->data[self->at];

  switch (b) {
  case 0xd4:
  case 0xd5:
  case 0xd6:
  case 0xd7:
  case 0xd8:
    *size = 1u << (b - 0xd4);
    self->at += 1;
    break;
  case 0xc7:
  case 0xc8:
  case 0xc9: {
    int width = 1 << (b - 0xc7);

    if (!need(self, 1 + (size_t)width))
      return 0;

    *size = (uint32_t)bigEndian(self->data + self->at + 1, width);
    self->at += 1 + (size_t)width;
    break;
  }
  default:
    return fail(self, "not an ext");
  }

  if (!need(self, 1 + (size_t)*size))
    return 0;

  *type = (int8_t)self->data[self->at];
  *data = self->data + self->at + 1;
  self->at += 1 + *size;

  return 1;
}

static int skipDepth(mp_reader_t *self, int depth) {

  uint32_t count;
  const char *text;
  const uint8_t *bytes;
  double d;
  int b;
  int8_t type;

  if (depth > 64)
    return fail(self, "nested too deep");

  switch (self->peek()) {
  case MP_NIL:
    return self->nil();
  case MP_BOOL:
    return self->boolean(&b);
  case MP_UINT:
  case MP_INT:
  case MP_FLOAT:
    return self->number(&d);
  case MP_STR:
    return self->str(&text, &count);
  case MP_BIN:
    return self->bin(&bytes, &count);
  case MP_EXT:
    return extension(self, &type, &bytes, &count);
  case MP_ARRAY:
    if (!self->array(&count))
      return 0;
    while (count-- > 0)
      if (!skipDepth(self, depth + 1))
        return 0;
    return 1;
  case MP_MAP:
    if (!self->map(&count))
      return 0;
    while (count-- > 0)
      if (!skipDepth(self, depth + 1) || !skipDepth(self, depth + 1))
        return 0;
    return 1;
  case MP_END:
    return fail(self, "truncated");
  default:
    return fail(self, "not msgpack");
  }
}

int mp_reader_t.skip(mp_reader_t *self) { return skipDepth(self, 0); }

/* -------------------------------------------------------------------- tree */

static yyjson_mut_val *build(yyjson_mut_doc *doc, mp_reader_t *r, int depth) {

  uint32_t count;
  const char *text;
  const uint8_t *bytes;
  int8_t type;
  int b;
  int kind;
  uint64_t u;
  int64_t s;
  double d;

  if (depth > 64) {
    fail(r, "nested too deep");
    return NULL;
  }

  switch (r->peek()) {
  case MP_NIL:
    return r->nil() ? yyjson_mut_null(doc) : NULL;
  case MP_BOOL:
    return r->boolean(&b) ? yyjson_mut_bool(doc, b) : NULL;
  case MP_UINT:
  case MP_INT:
  case MP_FLOAT:
    if (!numeric(r, &kind, &u, &s, &d))
      return NULL;
    if (kind == 'u')
      return yyjson_mut_uint(doc, u);
    if (kind == 's')
      return yyjson_mut_sint(doc, s);
    return yyjson_mut_real(doc, d);
  case MP_STR:
    return r->str(&text, &count) ? yyjson_mut_strncpy(doc, text, count) : NULL;
  case MP_BIN:
    return r->bin(&bytes, &count)
               ? yyjson_mut_rawncpy(doc, (const char *)bytes, count)
               : NULL;
  case MP_EXT:
    if (!extension(r, &type, &bytes, &count))
      return NULL;
    if (type == 0 && count == 1 && bytes[0] == 0)
      return yyjson_mut_null(doc);
    fail(r, "an ext other than msgpackr's undefined");
    return NULL;
  case MP_ARRAY: {
    yyjson_mut_val *array;

    if (!r->array(&count))
      return NULL;

    array = yyjson_mut_arr(doc);

    while (array != NULL && count-- > 0) {

      yyjson_mut_val *item = build(doc, r, depth + 1);

      if (item == NULL || !yyjson_mut_arr_append(array, item))
        return NULL;
    }

    return array;
  }
  case MP_MAP: {
    yyjson_mut_val *object;

    if (!r->map(&count))
      return NULL;

    object = yyjson_mut_obj(doc);

    while (object != NULL && count-- > 0) {

      yyjson_mut_val *key;
      yyjson_mut_val *value;
      uint32_t size;

      /**
       * A JS object turns every key into a string, so an integer key is
       * written out in decimal the way JS would. Anything else as a key is
       * not something these messages carry, and is refused.
       */
      if (r->peek() == MP_STR) {
        if (!r->str(&text, &size))
          return NULL;
        key = yyjson_mut_strncpy(doc, text, size);
      } else if (r->peek() == MP_UINT || r->peek() == MP_INT) {
        char digits[24];

        if (!numeric(r, &kind, &u, &s, &d))
          return NULL;
        if (kind == 'u')
          snprintf(digits, sizeof digits, "%llu", (unsigned long long)u);
        else
          snprintf(digits, sizeof digits, "%lld", (long long)s);
        key = yyjson_mut_strcpy(doc, digits);
      } else {
        fail(r, "a map key that is neither a str nor an integer");
        return NULL;
      }

      value = build(doc, r, depth + 1);

      if (key == NULL || value == NULL || !yyjson_mut_obj_add(object, key, value))
        return NULL;
    }

    return object;
  }
  default:
    fail(r, r->at >= r->len ? "truncated" : "not msgpack");
    return NULL;
  }
}

static json_t refused(const char *why) {

  json_t got = {NULL, NULL, NULL, "", NULL, 0, NULL};

  got._refused = why;

  return got;
}

META_OWNS json_t mp_decode(const void *data, size_t len) {

  mp_reader_t r = mp_reader(data, len);
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root;

  if (doc == NULL)
    return refused("there was no memory for the document");

  root = build(doc, &r, 0);

  if (root == NULL || r.error != NULL || r.at != r.len) {

    const char *why = r.error != NULL ? r.error
                      : root == NULL  ? "there was no memory for the document"
                                      : "bytes left over after the value";

    yyjson_mut_doc_free(doc);
    return refused(why);
  }

  yyjson_mut_doc_set_root(doc, root);

  return meta_jsonFromMut(doc);
}

static int encodeNode(mp_buf_t *into, yyjson_mut_val *node, int depth) {

  if (node == NULL || depth > 64)
    return 0;

  switch (yyjson_mut_get_type(node)) {
  case YYJSON_TYPE_NULL:
    into->nil();
    return 1;
  case YYJSON_TYPE_BOOL:
    into->boolean(yyjson_mut_get_bool(node));
    return 1;
  case YYJSON_TYPE_NUM:
    switch (yyjson_mut_get_subtype(node)) {
    case YYJSON_SUBTYPE_UINT: {
      uint64_t u = yyjson_mut_get_uint(node);

      into->number((double)u);
      return 1;
    }
    case YYJSON_SUBTYPE_SINT:
      into->integer(yyjson_mut_get_sint(node));
      return 1;
    default:
      into->number(yyjson_mut_get_real(node));
      return 1;
    }
  case YYJSON_TYPE_STR:
    into->str(yyjson_mut_get_str(node), yyjson_mut_get_len(node));
    return 1;
  case YYJSON_TYPE_RAW:
    into->bin(yyjson_mut_get_raw(node), yyjson_mut_get_len(node));
    return 1;
  case YYJSON_TYPE_ARR: {
    size_t at, count;
    yyjson_mut_val *item;

    into->array((uint32_t)yyjson_mut_arr_size(node));

    yyjson_mut_arr_foreach(node, at, count, item) {
      if (!encodeNode(into, item, depth + 1))
        return 0;
    }

    return 1;
  }
  case YYJSON_TYPE_OBJ: {
    size_t at, count;
    yyjson_mut_val *key;
    yyjson_mut_val *value;

    into->map((uint32_t)yyjson_mut_obj_size(node));

    yyjson_mut_obj_foreach(node, at, count, key, value) {
      into->str(yyjson_mut_get_str(key), yyjson_mut_get_len(key));
      if (!encodeNode(into, value, depth + 1))
        return 0;
    }

    return 1;
  }
  default:
    return 0;
  }
}

int mp_encode(mp_buf_t *into, json_t value) {

  if (value._node == NULL)
    return 0;

  return encodeNode(into, value._node, 0) && !into->failed;
}

/* -------------------------------------------------------------- typed dump */

/** JSON.stringify's escaping, which is what mpdump.js writes a str with. */
static void quoted(mp_buf_t *text, const char *at, size_t size) {

  text->byte('"');

  for (size_t i = 0; i < size; ++i) {

    unsigned char c = (unsigned char)at[i];
    char escape[8];

    switch (c) {
    case '"':
      text->text("\\\"");
      break;
    case '\\':
      text->text("\\\\");
      break;
    case '\b':
      text->text("\\b");
      break;
    case '\f':
      text->text("\\f");
      break;
    case '\n':
      text->text("\\n");
      break;
    case '\r':
      text->text("\\r");
      break;
    case '\t':
      text->text("\\t");
      break;
    default:
      if (c < 0x20) {
        snprintf(escape, sizeof escape, "\\u%04x", c);
        text->text(escape);
      } else
        text->byte(c);
    }
  }

  text->byte('"');
}

/** A whole number as one, anything else as its bits - nothing to round. */
static void floating(mp_buf_t *text, double value) {

  char out[40];

  if (value == floor(value) && fabs(value) <= 9007199254740991.0) {
    if (value == 0)
      snprintf(out, sizeof out, "f:0");
    else
      snprintf(out, sizeof out, "f:%.0f", value);
  } else {
    uint64_t bits;

    memcpy(&bits, &value, sizeof bits);
    snprintf(out, sizeof out, "f:0x%016llx", (unsigned long long)bits);
  }

  text->text(out);
}

static void hexOf(mp_buf_t *text, const uint8_t *bytes, size_t size) {

  static const char digits[] = "0123456789abcdef";

  for (size_t i = 0; i < size; ++i) {
    text->byte((uint8_t)digits[bytes[i] >> 4]);
    text->byte((uint8_t)digits[bytes[i] & 15]);
  }
}

static int dumpValue(mp_reader_t *r, mp_buf_t *text, int depth) {

  char out[40];
  uint32_t count;
  const char *s;
  const uint8_t *bytes;
  int8_t type;
  int b;
  int kind;
  uint64_t u;
  int64_t i;
  double d;

  if (depth > 64)
    return fail(r, "nested too deep");

  switch (r->peek()) {
  case MP_NIL:
    text->text("nil");
    return r->nil();
  case MP_BOOL:
    if (!r->boolean(&b))
      return 0;
    text->text(b ? "true" : "false");
    return 1;
  case MP_UINT:
  case MP_INT:
  case MP_FLOAT:
    if (!numeric(r, &kind, &u, &i, &d))
      return 0;
    if (kind == 'd')
      floating(text, d);
    else {
      if (kind == 'u')
        snprintf(out, sizeof out, "u:%llu", (unsigned long long)u);
      else
        snprintf(out, sizeof out, "%s:%lld", i < 0 ? "i" : "u", (long long)i);
      text->text(out);
    }
    return 1;
  case MP_STR:
    if (!r->str(&s, &count))
      return 0;
    quoted(text, s, count);
    return 1;
  case MP_BIN:
    if (!r->bin(&bytes, &count))
      return 0;
    text->text("bin:");
    hexOf(text, bytes, count);
    return 1;
  case MP_EXT:
    if (!extension(r, &type, &bytes, &count))
      return 0;
    if (type == 0 && count == 1 && bytes[0] == 0)
      text->text("nil");
    else {
      snprintf(out, sizeof out, "ext:%d:", type);
      text->text(out);
      hexOf(text, bytes, count);
    }
    return 1;
  case MP_ARRAY:
    if (!r->array(&count))
      return 0;
    text->byte('[');
    for (uint32_t at = 0; at < count; ++at) {
      if (at > 0)
        text->byte(',');
      if (!dumpValue(r, text, depth + 1))
        return 0;
    }
    text->byte(']');
    return 1;
  case MP_MAP:
    if (!r->map(&count))
      return 0;
    text->byte('{');
    for (uint32_t at = 0; at < count; ++at) {
      if (at > 0)
        text->byte(',');
      if (!dumpValue(r, text, depth + 1))
        return 0;
      text->byte(':');
      if (!dumpValue(r, text, depth + 1))
        return 0;
    }
    text->byte('}');
    return 1;
  default:
    return fail(r, r->at >= r->len ? "truncated" : "not msgpack");
  }
}

int mp_dump(const void *data, size_t len, mp_buf_t *text) {

  mp_reader_t r = mp_reader(data, len);

  if (!dumpValue(&r, text, 0) || r.at != r.len)
    return 0;

  return !text->failed;
}

static int dumpNode(yyjson_mut_val *node, mp_buf_t *text, int depth) {

  char out[40];

  if (node == NULL || depth > 64)
    return 0;

  switch (yyjson_mut_get_type(node)) {
  case YYJSON_TYPE_NULL:
    text->text("nil");
    return 1;
  case YYJSON_TYPE_BOOL:
    text->text(yyjson_mut_get_bool(node) ? "true" : "false");
    return 1;
  case YYJSON_TYPE_NUM:
    switch (yyjson_mut_get_subtype(node)) {
    case YYJSON_SUBTYPE_UINT:
      snprintf(out, sizeof out, "u:%llu",
               (unsigned long long)yyjson_mut_get_uint(node));
      break;
    case YYJSON_SUBTYPE_SINT: {
      long long v = (long long)yyjson_mut_get_sint(node);

      snprintf(out, sizeof out, "%s:%lld", v < 0 ? "i" : "u", v);
      break;
    }
    default:
      floating(text, yyjson_mut_get_real(node));
      return 1;
    }
    text->text(out);
    return 1;
  case YYJSON_TYPE_STR:
    quoted(text, yyjson_mut_get_str(node), yyjson_mut_get_len(node));
    return 1;
  case YYJSON_TYPE_RAW:
    text->text("bin:");
    hexOf(text, (const uint8_t *)yyjson_mut_get_raw(node),
          yyjson_mut_get_len(node));
    return 1;
  case YYJSON_TYPE_ARR: {
    size_t at, count;
    yyjson_mut_val *item;

    text->byte('[');
    yyjson_mut_arr_foreach(node, at, count, item) {
      if (at > 0)
        text->byte(',');
      if (!dumpNode(item, text, depth + 1))
        return 0;
    }
    text->byte(']');
    return 1;
  }
  case YYJSON_TYPE_OBJ: {
    size_t at, count;
    yyjson_mut_val *key;
    yyjson_mut_val *value;

    text->byte('{');
    yyjson_mut_obj_foreach(node, at, count, key, value) {
      if (at > 0)
        text->byte(',');
      quoted(text, yyjson_mut_get_str(key), yyjson_mut_get_len(key));
      text->byte(':');
      if (!dumpNode(value, text, depth + 1))
        return 0;
    }
    text->byte('}');
    return 1;
  }
  default:
    return 0;
  }
}

int mp_dumpTree(json_t value, mp_buf_t *text) {
  return dumpNode(value._node, text, 0) && !text->failed;
}
