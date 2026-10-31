/**
 * The SWIM core on a fake clock and a fake network.
 *
 * test/harness/interop.js is the proof against real Node nodes; this is the
 * place for the rules one at a time, where a run takes no wall time and
 * nothing is left to chance but rand(), which is seeded:
 *
 *   - captured datagrams split into the parts swim-ng put in them
 *   - what we send is byte for byte what swim-ng sends for the same thing
 *   - join, ping, ping-req, suspect, faulty with swim-ng's timings
 *   - the incarnation rules: refuting, alive-at-the-same-incarnation,
 *     the faulty record, a faulty about a stranger
 *   - dissemination: an update rides along ceil(22 * log10(n + 1)) times
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msgpack.h"
#include "swim.h"

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

  for (; hex[0] != 0 && hex[1] != 0 && hex[0] != '\t' && hex[0] != '\n';
       hex += 2) {
    unsigned v;
    sscanf(hex, "%2x", &v);
    into[n++] = (uint8_t)v;
  }

  return n;
}

static void hexOf(char *into, const uint8_t *bytes, size_t size) {

  for (size_t i = 0; i < size; ++i)
    sprintf(into + 2 * i, "%02x", bytes[i]);

  into[2 * size] = 0;
}

/* ------------------------------------------------------------- framing */

typedef struct {
  char *text;
  size_t len;
  size_t room;
} parts_t;

static void collect(void *context, int type, const uint8_t *body, size_t size) {

  parts_t *p = (parts_t *)context;
  const char *name = type == SWIM_PING       ? "Ping"
                     : type == SWIM_PING_REQ ? "PingReq"
                     : type == SWIM_SYNC     ? "Sync"
                     : type == SWIM_ACK      ? "Ack"
                     : type == SWIM_UPDATE   ? "Update"
                                             : "?";
  size_t need = p->len + strlen(name) + 2 * size + 4;

  if (need > p->room) {
    p->room = need * 2;
    p->text = (char *)realloc(p->text, p->room);
  }

  p->len += (size_t)sprintf(p->text + p->len, "%s%s:", p->len > 0 ? " " : "", name);
  hexOf(p->text + p->len, body, size);
  p->len += 2 * size;
}

static void frames(const char *dir) {

  char path[1024];
  FILE *in;
  char *line = NULL;
  size_t room = 0;
  int count = 0;

  snprintf(path, sizeof path, "%s/swim-frames.txt", dir);
  in = fopen(path, "r");

  if (in == NULL) {
    CHECK(0, "cannot read %s", path);
    return;
  }

  while (getline(&line, &room, in) > 0) {

    char *name = strtok(line, "\t");
    char *hex = strtok(NULL, "\t");
    char *want = strtok(NULL, "\n");
    uint8_t *bytes;
    size_t size;
    parts_t got = {NULL, 0, 0};

    if (name == NULL || hex == NULL || want == NULL)
      continue;

    bytes = (uint8_t *)malloc(strlen(hex) / 2 + 1);
    size = unhex(hex, bytes);

    CHECK(swim_frames(bytes, size, collect, &got) > 0, "%s: no frames", name);
    CHECK(got.text != NULL && strcmp(got.text, want) == 0, "%s: parts differ",
          name);

    free(got.text);
    free(bytes);
    ++count;
  }

  printf("swim-frames.txt: %d datagrams\n", count);
  free(line);
  fclose(in);
}

/* ------------------------------------------------------ a fake network */

#define NODES 4
#define QUEUE 4096

typedef struct {
  char to[SWIM_HOST_MAX];
  char from[SWIM_HOST_MAX];
  uint8_t *data;
  size_t size;
} packet_t;

typedef struct node_t {
  char host[SWIM_HOST_MAX];
  swim_t *swim;
  int up;
  int changes;
  int updates;
  int joined;
  int failed;
  char lastState[NODES * 2][SWIM_HOST_MAX];
} node_t;

static node_t nodes[NODES];
static packet_t queue[QUEUE];
static size_t queued;
static long clock_;

/* the last datagram each node sent, for byte checks */
static uint8_t lastSent[NODES][4096];
static size_t lastSentSize[NODES];
static char lastSentTo[NODES][SWIM_HOST_MAX];

static void netSend(void *context, const char *host, const uint8_t *data,
                    size_t size) {

  node_t *from = (node_t *)context;
  int index = (int)(from - nodes);

  if (size <= sizeof lastSent[0]) {
    memcpy(lastSent[index], data, size);
    lastSentSize[index] = size;
    snprintf(lastSentTo[index], SWIM_HOST_MAX, "%s", host);
  }

  if (queued == QUEUE)
    return;

  snprintf(queue[queued].to, SWIM_HOST_MAX, "%s", host);
  snprintf(queue[queued].from, SWIM_HOST_MAX, "%s", from->host);
  queue[queued].data = (uint8_t *)malloc(size);
  memcpy(queue[queued].data, data, size);
  queue[queued].size = size;
  queued += 1;
}

static void netEvent(void *context, swim_event_t event, const swim_member_t *m) {

  node_t *node = (node_t *)context;

  if (event == SWIM_EVENT_CHANGE)
    node->changes += 1;
  else if (event == SWIM_EVENT_UPDATE)
    node->updates += 1;
  else if (event == SWIM_EVENT_JOINED)
    node->joined += 1;
  else
    node->failed += 1;
}

/** Delivers everything queued, and what that sends, until it is quiet. */
static void flush(void) {

  for (int rounds = 0; queued > 0 && rounds < 100; ++rounds) {

    packet_t batch[QUEUE];
    size_t count = queued;

    memcpy(batch, queue, count * sizeof *batch);
    queued = 0;

    for (size_t i = 0; i < count; ++i) {
      for (int n = 0; n < NODES; ++n)
        if (nodes[n].swim != NULL && nodes[n].up &&
            strcmp(nodes[n].host, batch[i].to) == 0)
          swim_receive(nodes[n].swim, batch[i].data, batch[i].size,
                       batch[i].from, clock_);
      free(batch[i].data);
    }
  }
}

/** Runs the clock forward in small steps, delivering as it goes. */
static void runFor(long ms) {

  long until = clock_ + ms;

  while (clock_ < until) {

    clock_ += 1;

    for (int n = 0; n < NODES; ++n)
      if (nodes[n].swim != NULL && nodes[n].up)
        swim_advance(nodes[n].swim, clock_);

    flush();
  }
}

static uint8_t *metaFor(const char *name, size_t *size) {

  mp_buf_t b = {0};
  uint8_t *out;

  b.map(2);
  b.cstr("identifier$");
  b.cstr(name);
  b.cstr("tag$");
  b.cstr("mc~test");

  out = b.data;
  *size = b.len;

  return out;
}

static void bring(int n, int port, int64_t incarnation) {

  swim_hooks_t hooks;
  size_t size;
  uint8_t *meta;
  char name[32];

  snprintf(nodes[n].host, SWIM_HOST_MAX, "127.0.0.1:%d", port);
  snprintf(name, sizeof name, "node%d", n);
  meta = metaFor(name, &size);

  hooks.context = &nodes[n];
  hooks.send = netSend;
  hooks.event = netEvent;

  nodes[n].swim = swim_new(NULL, nodes[n].host, meta, size, incarnation, hooks);
  nodes[n].up = 1;
  free(meta);

  swim_start(nodes[n].swim, clock_);
}

static void dropAll(void) {

  for (int n = 0; n < NODES; ++n) {
    swim_free(nodes[n].swim);
    memset(&nodes[n], 0, sizeof nodes[n]);
  }

  for (size_t i = 0; i < queued; ++i)
    free(queue[i].data);

  queued = 0;
}

static const swim_member_t *seen(int by, int whom) {
  return swim_get(nodes[by].swim, nodes[whom].host);
}

/* -------------------------------------------------------------- the rules */

/** A Ping and a Sync, as swim-ng writes them. */
static void bytes(void) {

  char hex[8192];
  const char *base = "127.0.0.1:38999";
  mp_buf_t text = {0};

  clock_ = 0;
  bring(0, 40001, 1793405011551LL);

  /* a lone node joining a base it cannot reach: the Sync goes out */
  swim_join(nodes[0].swim, &base, 1, clock_);
  CHECK(lastSentSize[0] > 4 && strcmp(lastSentTo[0], base) == 0,
        "the Sync goes to the base");

  /* compound of one Sync, nothing else known yet: 00 0100 <len> 0c ... */
  CHECK(lastSent[0][0] == SWIM_COMPOUND && lastSent[0][1] == 1 &&
            lastSent[0][2] == 0 && lastSent[0][5] == SWIM_SYNC,
        "a Sync goes out as a compound of one, as Net.sendMessages makes it");

  CHECK(mp_dump(lastSent[0] + 6, lastSentSize[0] - 6, &text),
        "the Sync body is msgpack");
  mp_buf_t want = {0};
  want.text("{\"meta\":{\"identifier$\":\"node0\",\"tag$\":\"mc~test\"},"
            "\"host\":\"127.0.0.1:40001\",\"state\":u:0,"
            "\"incarnation\":f:1793405011551}");
  CHECK(text.len == want.len && memcmp(text.data, want.data, want.len) == 0,
        "Sync body: %.*s", (int)text.len, (const char *)text.data);
  want.release();

  /* the incarnation as Date.now() goes out: cb + IEEE double */
  hexOf(hex, lastSent[0], lastSentSize[0]);
  CHECK(strstr(hex, "ab696e6361726e6174696f6ecb427a18f44c65f000") != NULL,
        "incarnation is a float64, key and all");

  text.release();
  dropAll();
}

static void joinAndDetect(void) {

  const char *base;
  long t0;

  clock_ = 0;
  srand(1);
  bring(0, 38999, 1000);
  bring(1, 40001, 2000);
  bring(2, 40002, 3000);
  base = nodes[0].host;

  swim_join(nodes[0].swim, NULL, 0, clock_);
  CHECK(nodes[0].joined == 1, "a base with nobody else joins at once");

  swim_join(nodes[1].swim, &base, 1, clock_);
  swim_join(nodes[2].swim, &base, 1, clock_);
  runFor(200);

  CHECK(nodes[1].joined == 1 && nodes[2].joined == 1, "both joined the base");
  CHECK(seen(0, 1) && seen(0, 2), "the base knows both");
  CHECK(seen(1, 0), "1 knows the base");

  runFor(2000);
  CHECK(seen(1, 2) && seen(2, 1), "1 and 2 found each other through gossip");

  /* the ping always gets its ack: nobody is suspected while all are up */
  CHECK(seen(0, 1)->state == SWIM_ALIVE && seen(0, 2)->state == SWIM_ALIVE,
        "all alive");

  /* 2 dies: suspect within interval * n + 444 + 555, faulty 999 later */
  nodes[2].up = 0;
  t0 = clock_;

  while (clock_ - t0 < 6000 && seen(0, 2) != NULL &&
         seen(0, 2)->state == SWIM_ALIVE)
    runFor(1);

  CHECK(seen(0, 2) != NULL && seen(0, 2)->state == SWIM_SUSPECT,
        "the base suspects 2");
  CHECK(clock_ - t0 <= 4 * 333 + 444 + 555 + 2,
        "suspect after a ping, its timeout and the ping-req timeout (%ld ms)",
        clock_ - t0);

  t0 = clock_;

  while (clock_ - t0 < 3000 && seen(0, 2) != NULL)
    runFor(1);

  CHECK(seen(0, 2) == NULL, "2 is faulty for the base");
  CHECK(clock_ - t0 <= 999 + 1, "faulty one suspectTimeout later (%ld ms)",
        clock_ - t0);

  runFor(500);
  CHECK(seen(1, 2) == NULL, "and for 1, through dissemination");

  /* 2 comes back at the same host with a newer incarnation: alive again */
  swim_free(nodes[2].swim);
  nodes[2].swim = NULL;
  bring(2, 40002, 9000);
  swim_join(nodes[2].swim, &base, 1, clock_);
  runFor(1500);
  CHECK(seen(0, 2) && seen(0, 2)->incarnation == 9000 &&
            seen(0, 2)->state == SWIM_ALIVE,
        "restarted with a higher incarnation, alive again");

  dropAll();
}

/** An Update body for `host` in `state` at `incarnation`. */
static size_t update(uint8_t *into, const char *host, int state,
                     int64_t incarnation) {

  mp_buf_t b = {0};
  size_t size;

  b.byte(SWIM_UPDATE);
  b.map(4);
  b.cstr("meta");
  b.map(0);
  b.cstr("host");
  b.cstr(host);
  b.cstr("state");
  b.integer(state);
  b.cstr("incarnation");
  b.integer(incarnation);

  memcpy(into, b.data, b.len);
  size = b.len;
  b.release();

  return size;
}

static void incarnations(void) {

  uint8_t d[512];
  size_t n;
  const char *stranger = "127.0.0.1:41111";

  clock_ = 0;
  bring(0, 40000, 5000);

  /* suspected at our own incarnation: refute with one more */
  n = update(d, nodes[0].host, SWIM_SUSPECT, 5000);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_local(nodes[0].swim)->incarnation == 5001,
        "suspect at 5000 makes us 5001");

  /* suspected at an older one: no change, but the update still goes round */
  {
    int before = nodes[0].updates;

    n = update(d, nodes[0].host, SWIM_SUSPECT, 10);
    swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
    CHECK(swim_local(nodes[0].swim)->incarnation == 5001,
          "an old suspect changes nothing");
    CHECK(nodes[0].updates == before + 1, "but we say we are alive anyway");
  }

  /* alive about us with a higher incarnation: take it and add one */
  n = update(d, nodes[0].host, SWIM_ALIVE, 7000);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_local(nodes[0].swim)->incarnation == 7001, "alive at 7000 makes us 7001");

  /* a faulty about somebody we never heard of is dropped */
  n = update(d, stranger, SWIM_FAULTY, 1);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_get(nodes[0].swim, stranger) == NULL, "faulty stranger dropped");

  n = update(d, stranger, SWIM_ALIVE, 100);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_get(nodes[0].swim, stranger) != NULL, "and taken in when alive");

  /* a suspect at the same incarnation is taken, an alive at the same is not */
  n = update(d, stranger, SWIM_SUSPECT, 100);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_get(nodes[0].swim, stranger)->state == SWIM_SUSPECT,
        "suspect at the same incarnation wins over alive");

  n = update(d, stranger, SWIM_ALIVE, 100);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_get(nodes[0].swim, stranger)->state == SWIM_SUSPECT,
        "alive at the same incarnation does not lift it");

  n = update(d, stranger, SWIM_ALIVE, 101);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_get(nodes[0].swim, stranger)->state == SWIM_ALIVE,
        "a higher one does");

  /* hearing a suspect starts no timer: only our own failed pings do */
  n = update(d, stranger, SWIM_SUSPECT, 101);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  {
    long deadline = swim_nextDeadline(nodes[0].swim);
    CHECK(deadline <= clock_ + 333, "only the tick and pings are due");
  }

  /* faulty at the same incarnation: gone, and its record holds */
  n = update(d, stranger, SWIM_FAULTY, 101);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_get(nodes[0].swim, stranger) == NULL, "faulty at the same incarnation");

  n = update(d, stranger, SWIM_ALIVE, 101);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_get(nodes[0].swim, stranger) == NULL,
        "alive at the faulty incarnation is dropped");

  n = update(d, stranger, SWIM_SUSPECT, 101);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_get(nodes[0].swim, stranger) == NULL, "so is suspect");

  n = update(d, stranger, SWIM_ALIVE, 102);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);
  CHECK(swim_get(nodes[0].swim, stranger) != NULL, "a restart with more comes back");

  dropAll();
}

/** An Ack answers where the Ping came from, not the member's own host. */
static void replies(void) {

  uint8_t d[64];
  size_t n;
  mp_buf_t b = {0};

  clock_ = 0;
  bring(0, 40000, 1);

  b.byte(SWIM_PING);
  b.map(1);
  b.cstr("seq");
  b.integer(77);
  memcpy(d, b.data, b.len);
  n = b.len;
  b.release();

  swim_receive(nodes[0].swim, d, n, "127.0.0.1:5555", clock_);
  CHECK(strcmp(lastSentTo[0], "127.0.0.1:5555") == 0, "ack to the sender");
  {
    char hex[64];
    hexOf(hex, lastSent[0], lastSentSize[0]);
    CHECK(strcmp(hex, "0dde0001a37365714d") == 0, "Ack {seq: 77} alone: %s", hex);
  }

  dropAll();
}

/** ceil(22 * log10(size + 1)) tellings of each update, then it is dropped. */
static void dissemination(void) {

  uint8_t d[512];
  size_t n;
  int tellings = 0;
  const char *other = "127.0.0.1:41000";

  clock_ = 0;
  bring(0, 40000, 1);

  /* one other member: size 2 with us, ceil(22 * log10(3)) = 11 */
  n = update(d, other, SWIM_ALIVE, 5);
  swim_receive(nodes[0].swim, d, n, "127.0.0.1:1", clock_);

  for (int i = 0; i < 40; ++i) {

    uint8_t ping[16];
    size_t pn;
    mp_buf_t b = {0};

    b.byte(SWIM_PING);
    b.map(1);
    b.cstr("seq");
    b.integer(i);
    memcpy(ping, b.data, b.len);
    pn = b.len;
    b.release();

    swim_receive(nodes[0].swim, ping, pn, "127.0.0.1:2", clock_);

    /* the ack carries the update while it has tellings left */
    if (lastSent[0][0] == SWIM_COMPOUND)
      tellings += 1;
  }

  CHECK(tellings == 11, "an update rides along 11 times with 2 members, got %d",
        tellings);

  dropAll();
}

int main(int argc, char **argv) {

  const char *dir = argc > 1 ? argv[1] : "test/fixtures";

  frames(dir);
  bytes();
  joinAndDetect();
  incarnations();
  replies();
  dissemination();

  if (failures != 0) {
    printf("swim: %d of %d checks failed\n", failures, checked);
    return 1;
  }

  printf("swim: %d checks ok\n", checked);
  return 0;
}
