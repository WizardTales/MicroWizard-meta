/**
 * The msgpack codec against bytes msgpackr wrote.
 *
 * Two golden files, both <name> TAB <hex> TAB <typed dump>:
 *
 *   msgpackr-samples.txt   what msgpackr 1.11.2's default pack makes of edge
 *                          values (test/harness/pack-samples.js)
 *   swim-bodies.txt        message bodies captured off real Node nodes
 *                          (test/harness/capture.js, fixtures.js)
 *
 * For each: the dump written straight from the bytes must match mpdump.js's,
 * the decoded document must dump the same, and encoding that document must
 * give the same bytes back - except for the few values JS itself would not
 * write that way (a BigInt, undefined), which are only decoded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msgpack.h"

static int failures;
static int checked;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    ++checked;                                                                 \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                     \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static size_t unhex(const char *hex, uint8_t *into) {

  size_t n = 0;

  for (; hex[0] != 0 && hex[1] != 0; hex += 2) {
    unsigned v;
    sscanf(hex, "%2x", &v);
    into[n++] = (uint8_t)v;
  }

  return n;
}

static int decodeOnly(const char *name) {
  return strncmp(name, "bigint-", 7) == 0 || strcmp(name, "undefined") == 0;
}

static void golden(const char *dir, const char *file) {

  char path[1024];
  FILE *in;
  char *line = NULL;
  size_t room = 0;
  int count = 0;

  snprintf(path, sizeof path, "%s/%s", dir, file);
  in = fopen(path, "r");

  if (in == NULL) {
    CHECK(0, "cannot read %s", path);
    return;
  }

  while (getline(&line, &room, in) > 0) {

    char *name = strtok(line, "\t");
    char *hex = strtok(NULL, "\t");
    char *dump = strtok(NULL, "\n");
    uint8_t *bytes;
    size_t size;
    mp_buf_t text = {0};
    mp_buf_t back = {0};

    if (name == NULL || hex == NULL || dump == NULL)
      continue;

    bytes = (uint8_t *)malloc(strlen(hex) / 2 + 1);
    size = unhex(hex, bytes);
    ++count;

    CHECK(mp_dump(bytes, size, &text), "%s: not one msgpack value", name);
    CHECK(text.len == strlen(dump) && memcmp(text.data, dump, text.len) == 0,
          "%s: dump from the bytes\n  got  %.*s\n  want %.200s", name,
          (int)(text.len > 200 ? 200 : text.len), (const char *)text.data, dump);

    {
      json_t tree = mp_decode(bytes, size);

      CHECK(tree._refused == NULL, "%s: decode refused: %s", name,
            tree._refused != NULL ? tree._refused : "");

      if (tree._refused == NULL) {

        text.reset();
        CHECK(mp_dumpTree(tree, &text), "%s: tree dump failed", name);
        CHECK(text.len == strlen(dump) && memcmp(text.data, dump, text.len) == 0,
              "%s: dump from the tree\n  got  %.*s\n  want %.200s", name,
              (int)(text.len > 200 ? 200 : text.len), (const char *)text.data,
              dump);

        if (!decodeOnly(name)) {
          CHECK(mp_encode(&back, tree), "%s: encode failed", name);
          CHECK(back.len == size && memcmp(back.data, bytes, size) == 0,
                "%s: encoded back to different bytes (%zu against %zu)", name,
                back.len, size);
        }
      }

      tree.release();
    }

    text.release();
    back.release();
    free(bytes);
  }

  printf("%s: %d values\n", file, count);
  free(line);
  fclose(in);
}

static int bytesAre(mp_buf_t *buf, const char *hex) {

  uint8_t want[64];
  size_t n = unhex(hex, want);

  return buf->len == n && memcmp(buf->data, want, n) == 0;
}

/** The writer on its own: the JS number rule and the incarnation. */
static void writer(void) {

  mp_buf_t b = {0};

  b.number(1793405011551.0);
  CHECK(bytesAre(&b, "cb427a18f44c65f000"), "Date.now() is a float64");

  b.reset();
  b.integer(1793405011551LL);
  CHECK(bytesAre(&b, "cb427a18f44c65f000"), "an int64 incarnation is a float64 too");

  b.reset();
  b.integer(4294967295LL);
  CHECK(bytesAre(&b, "ceffffffff"), "uint32 max stays a uint32");

  b.reset();
  b.integer(-2147483649LL);
  CHECK(bytesAre(&b, "cbc1e0000000200000"), "below int32 is a float64");

  b.reset();
  b.number(-0.0);
  CHECK(bytesAre(&b, "00"), "-0 is fixint 0, as in msgpackr");

  b.reset();
  b.number(127.0);
  CHECK(bytesAre(&b, "7f"), "127 is a fixint");

  b.reset();
  b.map(1);
  b.cstr("seq");
  b.integer(0);
  CHECK(bytesAre(&b, "de0001a373657100"), "{seq:0} as swim-ng writes a Ping");

  /* array32, which no golden file carries */
  b.reset();
  b.array(70000);
  for (int i = 0; i < 70000; ++i)
    b.integer(1);
  {
    json_t tree = mp_decode(b.data, b.len);
    mp_buf_t again = {0};

    CHECK(tree._refused == NULL && tree.count() == 70000, "array32 decodes");
    CHECK(mp_encode(&again, tree) && again.len == b.len &&
              memcmp(again.data, b.data, b.len) == 0,
          "array32 round trips");
    again.release();
    tree.release();
  }

  b.release();
}

/** The cursor, the way swim.c reads an Update. */
static void reader(void) {

  uint8_t bytes[64];
  size_t n = unhex("de0004a573746174650aab696e6361726e6174696f6ecb427a18f44c65f000"
                   "a4686f7374a1789201c0",
                   bytes);
  mp_reader_t r = mp_reader(bytes, n);
  uint32_t count;
  int64_t value;
  const char *text;
  uint32_t size;

  CHECK(r.map(&count) && count == 4, "map16 of 4");
  CHECK(r.key("state") && r.integer(&value) && value == 10, "state");
  CHECK(!r.key("nope") && r.key("incarnation"), "key leaves a mismatch alone");
  CHECK(r.integer(&value) && value == 1793405011551LL,
        "a float64 incarnation reads as an int64");
  CHECK(r.str(&text, &size) && size == 4 && memcmp(text, "host", 4) == 0, "str");
  CHECK(r.str(&text, &size) && size == 1 && text[0] == 'x', "str value");
  CHECK(r.skip() && r.at == n && r.error == NULL, "skip over an array");

  /* a float that is not whole is not an integer, and the cursor stays */
  n = unhex("cb3ff8000000000000", bytes);
  r = mp_reader(bytes, n);
  CHECK(!r.integer(&value) && r.at == 0, "1.5 is not an integer");

  /* truncation is refused, not read past */
  n = unhex("de0002a3736571", bytes);
  r = mp_reader(bytes, n);
  CHECK(!r.skip() && r.error != NULL, "a truncated map is refused");

  {
    json_t tree = mp_decode(bytes, n);
    CHECK(tree._refused != NULL, "decode refuses it too");
    tree.release();
  }

  /* a body is exactly one value */
  n = unhex("c0c0", bytes);
  {
    json_t tree = mp_decode(bytes, n);
    CHECK(tree._refused != NULL, "trailing bytes are refused");
    tree.release();
  }
}

int main(int argc, char **argv) {

  const char *dir = argc > 1 ? argv[1] : "test/fixtures";

  golden(dir, "msgpackr-samples.txt");
  golden(dir, "swim-bodies.txt");
  writer();
  reader();

  if (failures != 0) {
    printf("msgpack: %d of %d checks failed\n", failures, checked);
    return 1;
  }

  printf("msgpack: %d checks ok\n", checked);
  return 0;
}
