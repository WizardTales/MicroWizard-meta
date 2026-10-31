/**
 * The meta visigoth against the JS one.
 *
 * test/fixtures/visigoth-trace.txt is what the real @wzrdtales/visigoth
 * decided for a few thousand adds, removes, choices, failures and clock steps
 * (test/harness/visigoth-trace.js writes it). This replays the same sequence
 * through src/visigoth.c and wants the same answer and the same ring, in the
 * same order with the same states, after every single step.
 *
 * Then a few sequences written out by hand, for what the trace shows only in
 * passing: the half-open target closed on being chosen, and balance.js's
 * retry loop running out of targets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "visigoth.h"

static int failures;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                     \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static void ringOf(vg_t *vg, char *into, size_t room) {

  size_t at = 0;

  into[0] = 0;

  if (vg->length == 0) {
    snprintf(into, room, "-");
    return;
  }

  for (vg_node_t *e = vg->top; e != NULL && at < room; e = e->next)
    at += (size_t)snprintf(into + at, room - at, "%s%s:%s", at > 0 ? " " : "",
                           (const char *)e->target, vg_statusName(e->status));
}

static int replay(const char *path) {

  FILE *in = fopen(path, "r");
  char *line = NULL;
  size_t room = 0;
  long clock = 1000000;
  int steps = 0;
  vg_t vg;
  vg_node_t *[char *] byName;
  char *[] names;

  if (in == NULL) {
    fprintf(stderr, "cannot read %s\n", path);
    return 1;
  }

  vg.init(VG_MICROWIZARD_CLOSING_TIMEOUT, 6);

  while (getline(&line, &room, in) > 0) {

    char *op = strtok(line, "\t");
    char *want = strtok(NULL, "\t");
    char *wantRing = strtok(NULL, "\t\n");
    char result[64] = "ok";
    char ring[1024];
    char *arg;

    if (op == NULL || want == NULL || wantRing == NULL)
      continue;

    ++steps;
    arg = strchr(op, ' ');
    if (arg != NULL)
      *arg++ = 0;

    if (strcmp(op, "add") == 0) {

      char *name = strdup(arg);
      vg_node_t *e = vg.add(name, 1, clock);

      names.push(name);

      if (e != NULL)
        byName.set(name, e);
      else
        snprintf(result, sizeof result, "full");

    } else if (strcmp(op, "remove") == 0) {

      vg_node_t **e = byName.get(arg);

      CHECK(e != NULL, "step %d: remove of unknown %s", steps, arg);
      if (e != NULL) {
        vg.remove(*e);
        byName.remove(arg);
      }

    } else if (strcmp(op, "choose") == 0) {

      vg_node_t *e = vg.choose(clock);

      snprintf(result, sizeof result, "%s",
               e != NULL ? (const char *)e->target : "none");

    } else if (strcmp(op, "fail") == 0) {

      vg_node_t **e = byName.get(arg);

      if (e != NULL)
        (*e)->failed(clock);

    } else if (strcmp(op, "time") == 0) {
      clock += atol(arg);
    }

    ringOf(&vg, ring, sizeof ring);

    CHECK(strcmp(result, want) == 0, "step %d (%s %s): got %s, visigoth said %s",
          steps, op, arg != NULL ? arg : "", result, want);
    CHECK(strcmp(ring, wantRing) == 0, "step %d (%s): ring\n  got  %s\n  want %s",
          steps, op, ring, wantRing);

    if (failures > 5)
      break;
  }

  printf("visigoth trace: %d steps replayed\n", steps);

  free(line);
  fclose(in);
  vg.release();
  byName.release();

  for (at in names)
    free(*at);

  names.release();

  return steps > 0 ? 0 : 1;
}

static void halfOpen(void) {

  vg_t vg;
  char a[] = "a", b[] = "b";
  vg_node_t *na, *nb;

  vg.init(VG_MICROWIZARD_CLOSING_TIMEOUT, 0);

  na = vg.add(a, 1, 0);
  nb = vg.add(b, 1, 0);

  /* round robin in insertion order */
  CHECK(vg.choose(0) == na, "first choice is a");
  CHECK(vg.choose(0) == nb, "second choice is b");
  CHECK(vg.choose(0) == na, "third choice is a again");

  /* a fails once and is out at once - no count, no threshold */
  na->failed(10);
  CHECK(vg.choose(20) == nb, "a open, b chosen");
  CHECK(vg.choose(20) == nb, "a open, b chosen again");

  /* exactly closingTimeout later it is still open: the test is strictly more */
  CHECK(vg.choose(1010) == nb || na->status == VG_OPEN, "still open at +1000");
  CHECK(na->status == VG_OPEN, "a open at exactly +1000");

  /* past it, a is half-open when reached - and closed on being chosen */
  {
    vg_node_t *first = vg.choose(1011);
    vg_node_t *second = vg.choose(1011);

    CHECK(first == na || second == na, "a chosen again after the timeout");
    CHECK(na->status == VG_CLOSED, "a closed by being chosen, before any answer");
  }

  /* both open: nothing to choose, which balance.js answers no-current-target */
  na->failed(2000);
  nb->failed(2000);
  CHECK(vg.choose(2500) == NULL, "all open means no upstream");

  vg.release();
}

/**
 * balance.js's consume loop with one target that is overloaded: the first
 * try fails and opens it, the retry 100 ms later finds nothing and the caller
 * gets no-current-target - not ten tries. Ten tries happen only with enough
 * targets to go round.
 */
static int tries(int targets, int failing) {

  vg_t vg;
  char names[8][4];
  long clock = 0;
  int attempts = 0;

  vg.init(VG_MICROWIZARD_CLOSING_TIMEOUT, 0);

  for (int i = 0; i < targets; ++i) {
    snprintf(names[i], sizeof names[i], "t%d", i);
    vg.add(names[i], 1, 0);
  }

  for (;;) {

    vg_node_t *e = vg.choose(clock);

    if (e == NULL)
      break;

    ++attempts;

    if (atoi((const char *)e->target + 1) >= failing)
      break; /* answered */

    e->failed(clock);

    if (attempts >= 10)
      break; /* all-targets-overloaded */

    clock += 100;
  }

  vg.release();

  return attempts;
}

int main(int argc, char **argv) {

  char path[1024];
  const char *dir = argc > 1 ? argv[1] : "test/fixtures";

  snprintf(path, sizeof path, "%s/visigoth-trace.txt", dir);

  if (replay(path) != 0)
    ++failures;

  halfOpen();

  CHECK(tries(1, 1) == 1, "one overloaded target: one try, then nothing to choose");
  CHECK(tries(3, 3) == 3, "three overloaded targets: three tries");
  CHECK(tries(3, 1) == 2, "one of three overloaded: the second try answers");
  CHECK(tries(8, 8) == 8, "eight overloaded: eight tries, the ring runs dry");

  if (failures != 0) {
    printf("visigoth: %d failure(s)\n", failures);
    return 1;
  }

  printf("visigoth: ok\n");
  return 0;
}
