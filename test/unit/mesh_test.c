/**
 * sneeze's member table, rule by rule, with the restart rules of 0.13.1.
 *
 * Members are made the way swim hands them over: a host, a state, an
 * incarnation and the meta as msgpack. What the table says back is a log
 * of "+id" and "-id", compared as a string.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mesh.h"
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

static char said[4096];

static void heard(void *context, int added, const mesh_peer_t *peer) {

  size_t at = strlen(said);

  snprintf(said + at, sizeof said - at, "%s%c%s", at != 0 ? " " : "",
           added ? '+' : '-', peer->identifier);
}

static void tell(mesh_t *mesh, const char *host, swim_state_t state,
                 int64_t incarnation, const char *identifier, const char *tag,
                 int version) {

  mp_buf_t meta = {0};
  swim_member_t m;

  meta.map(3);
  meta.cstr("identifier$");
  meta.cstr(identifier);
  meta.cstr("tag$");
  meta.cstr(tag);
  meta.cstr("v$");
  meta.integer(version);

  memset(&m, 0, sizeof m);
  snprintf(m.host, sizeof m.host, "%s", host);
  m.meta = meta.data;
  m.metaLen = meta.len;
  m.state = state;
  m.incarnation = incarnation;

  mesh_update(mesh, &m);
  meta.release();
}

#define SAYS(expected)                                                         \
  do {                                                                         \
    CHECK(strcmp(said, expected) == 0, "said \"%s\", wanted \"%s\"", said,     \
          expected);                                                           \
    said[0] = 0;                                                               \
  } while (0)

#define T "mc~t"

int main(void) {

  mesh_t mesh;

  mesh_init(&mesh, T, "me", heard, NULL);

  /* alive adds, once; the same news again is nothing */
  tell(&mesh, "127.0.0.1:1", SWIM_ALIVE, 100, "a1", T, 0);
  tell(&mesh, "127.0.0.1:1", SWIM_ALIVE, 100, "a1", T, 0);
  SAYS("+a1");

  /* another tag, ourselves, no identifier: passed over */
  tell(&mesh, "127.0.0.1:2", SWIM_ALIVE, 100, "x", "mc~other", 0);
  tell(&mesh, "127.0.0.1:3", SWIM_ALIVE, 100, "me", T, 0);
  tell(&mesh, "127.0.0.1:4", SWIM_ALIVE, 100, "", T, 0);
  SAYS("");

  /* a newer incarnation, a moved v$: added again (sneeze emits add) */
  tell(&mesh, "127.0.0.1:1", SWIM_ALIVE, 101, "a1", T, 0);
  tell(&mesh, "127.0.0.1:1", SWIM_ALIVE, 101, "a1", T, 1);
  SAYS("+a1 +a1");

  /* suspect says nothing */
  tell(&mesh, "127.0.0.1:1", SWIM_SUSPECT, 101, "a1", T, 1);
  SAYS("");

  /**
   * The restart: a1 is back at the same host as a2, newer. The old one goes
   * before the new one comes - and only once.
   */
  tell(&mesh, "127.0.0.1:1", SWIM_ALIVE, 200, "a2", T, 0);
  SAYS("-a1 +a2");
  CHECK(mesh_find(&mesh, "a1") == NULL, "a1 still held");
  CHECK(mesh_find(&mesh, "a2") != NULL, "a2 not held");

  /* late news about a1 at that host, older: it does not come back */
  tell(&mesh, "127.0.0.1:1", SWIM_ALIVE, 150, "a1", T, 0);
  SAYS("");

  /* a faulty for a1 there now removes nothing: a1 is not held */
  tell(&mesh, "127.0.0.1:1", SWIM_FAULTY, 150, "a1", T, 0);
  SAYS("");

  /**
   * Same incarnation at the same host under another identifier: replaces,
   * since only a *newer* one held there makes the news stale.
   */
  tell(&mesh, "127.0.0.1:1", SWIM_ALIVE, 200, "a3", T, 0);
  SAYS("-a2 +a3");

  /**
   * An abandoned join attempt of b: same identifier, another host, older
   * incarnation. Neither its alive nor its faulty touches the live b.
   */
  tell(&mesh, "127.0.0.1:5", SWIM_ALIVE, 500, "b", T, 0);
  SAYS("+b");
  tell(&mesh, "127.0.0.1:6", SWIM_ALIVE, 400, "b", T, 0);
  tell(&mesh, "127.0.0.1:6", SWIM_FAULTY, 400, "b", T, 0);
  SAYS("");
  CHECK(strcmp(mesh_find(&mesh, "b")->host, "127.0.0.1:5") == 0,
        "b moved to %s", mesh_find(&mesh, "b")->host);

  /* b at another host, newer: moved (an add, as sneeze emits one) */
  tell(&mesh, "127.0.0.1:6", SWIM_ALIVE, 600, "b", T, 0);
  SAYS("+b");

  /* faulty at the host it is held at removes */
  tell(&mesh, "127.0.0.1:6", SWIM_FAULTY, 600, "b", T, 0);
  SAYS("-b");

  /* a faulty for a stranger is nothing */
  tell(&mesh, "127.0.0.1:7", SWIM_FAULTY, 1, "nobody", T, 0);
  SAYS("");

  /* a member without meta is passed over */
  {
    swim_member_t bare;

    memset(&bare, 0, sizeof bare);
    snprintf(bare.host, sizeof bare.host, "127.0.0.1:8");
    mesh_update(&mesh, &bare);
    SAYS("");
  }

  mesh_free(&mesh);

  if (failures != 0) {
    fprintf(stderr, "mesh: %d of %d checks failed\n", failures, checked);
    return 1;
  }

  printf("mesh: %d checks ok\n", checked);
  return 0;
}
