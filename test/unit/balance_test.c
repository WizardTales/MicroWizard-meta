/**
 * balance.js consume on a fake clock, with the visigoth port choosing: the
 * cases that decide what a caller gets, as 1.6.2 has them, and the same
 * cases with the patient option, which is on by default - with the budget
 * of test/balance.test.js on microwizard's fix/consume-wait too.
 *
 * A target here is a script: what each call to it answers and how long it
 * takes. The run writes a log of what happened - "A+0" a call to A that
 * started at 0, "!A@5555" A opened at 5555 - and ends with the outcome and
 * the time, compared as a string.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "balance.h"
#include "visigoth.h"

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

/** One call's answer: 'o' ok, 'v' overload, 't' timeout, 'e' an error. */
typedef struct {
  char name;
  const char *script; /* one letter per call, the last one repeats */
  int calls;
  long took;          /* how long an answer takes; a timeout takes the timeout */
} fake_t;

static char logged[2048];

static void note(const char *format, long a, char c) {

  size_t at = strlen(logged);

  snprintf(logged + at, sizeof logged - at, format, c, a);
}

/**
 * Runs one call to the end against the targets, as client.c runs it: choose,
 * call, tell visigoth about a retryable failure, ask balance what next.
 */
static void runWith(fake_t *targets, int count, bal_options_t options,
                    long targetTimeout, char *out, size_t room) {

  vg_t ring;
  bal_consume_t call;
  long now = 0;

  logged[0] = 0;
  ring.init(options.closingTimeout, 0);

  for (int i = 0; i < count; ++i) {
    targets[i].calls = 0;
    ring.add(&targets[i], 1, now);
  }

  bal_begin(&call, &options, now);

  for (;;) {

    vg_node_t *node;
    fake_t *t;
    char what;
    size_t len;
    long timeout;
    bal_result_t result;

    now += call.wait;
    node = ring.choose(now);

    if (node == NULL) {
      if (bal_noTarget(&call, now) == BAL_DONE)
        break;
      continue;
    }

    t = (fake_t *)node->target;
    len = strlen(t->script);
    what = t->script[t->calls < (int)len ? t->calls : (int)len - 1];
    t->calls += 1;
    note(" %c+%ld", now, t->name);

    timeout = bal_tryTimeout(&call, targetTimeout, now);

    if (timeout == 0) {
      call.outcome = BAL_ALL_OVERLOADED;
      break;
    }

    if (what == 't') {
      now += timeout;
      result = BAL_RETRYABLE;
    } else {
      now += t->took;
      result = what == 'o' ? BAL_OK : what == 'v' ? BAL_RETRYABLE : BAL_ERROR;
    }

    if (result == BAL_RETRYABLE) {
      node->failed(now);
      note(" !%c@%ld", now, t->name);
    }

    if (bal_tried(&call, result, now) == BAL_DONE)
      break;
  }

  snprintf(out, room, "%s = %s@%ld", logged + 1, bal_outcomeText(call.outcome),
           now);
  ring.release();
}

static void run(fake_t *targets, int count, int patient, char *out,
                size_t room) {

  bal_options_t options;

  bal_defaults(&options);
  options.patient = patient;
  runWith(targets, count, options, 5555, out, room);
}

/** Patient, with balance.js's numbers changed: a budget, the circuit breaker. */
#define RUNS_WITH(targets, BUDGET, CLOSING, RETRY, TIMEOUT, expected)          \
  do {                                                                         \
    char got[2048];                                                            \
    bal_options_t options;                                                     \
    bal_defaults(&options);                                                    \
    options.budget = BUDGET;                                                   \
    options.closingTimeout = CLOSING;                                          \
    options.retryTimeout = RETRY;                                              \
    runWith(targets, (int)(sizeof targets / sizeof targets[0]), options,       \
            TIMEOUT, got, sizeof got);                                         \
    CHECK(strcmp(got, expected) == 0, "\n  got    %s\n  wanted %s", got,       \
          expected);                                                           \
  } while (0)

#define RUNS(targets, patient, expected)                                       \
  do {                                                                         \
    char got[2048];                                                            \
    run(targets, (int)(sizeof targets / sizeof targets[0]), patient, got,      \
        sizeof got);                                                           \
    CHECK(strcmp(got, expected) == 0, "\n  got    %s\n  wanted %s", got,       \
          expected);                                                           \
  } while (0)

int main(void) {

  /* --------------------------------------------------- as 1.6.2 has it */

  {
    /* answered at once */
    fake_t one[] = {{'A', "o", 0, 5}};
    RUNS(one, 0, "A+0 = answered@5");
  }

  {
    /* one target, overloaded: no ten tries - opened, then nothing to choose */
    fake_t one[] = {{'A', "v", 0, 5}};
    RUNS(one, 0, "A+0 !A@5 = no-current-target@105");
  }

  {
    /* the same with a timeout: 5555, the retry finds nothing */
    fake_t one[] = {{'A', "t", 0, 5}};
    RUNS(one, 0, "A+0 !A@5555 = no-current-target@5655");
  }

  {
    /* an error the service threw is its answer: no retry, nothing opened */
    fake_t two[] = {{'A', "e", 0, 5}, {'B', "o", 0, 5}};
    RUNS(two, 0, "A+0 = answered@5");
  }

  {
    /* failover: the retry goes to the other one, 100 ms after */
    fake_t two[] = {{'A', "v", 0, 5}, {'B', "o", 0, 5}};
    RUNS(two, 0, "A+0 !A@5 B+105 = answered@110");
  }

  {
    /* a dead target times out on connect (355), the other answers */
    fake_t two[] = {{'A', "t", 0, 5}, {'B', "o", 0, 5}};
    RUNS(two, 0, "A+0 !A@5555 B+5655 = answered@5660");
  }

  {
    /* two, both overloaded: both opened inside a second, then nothing */
    fake_t two[] = {{'A', "v", 0, 5}, {'B', "v", 0, 5}};
    RUNS(two, 0, "A+0 !A@5 B+105 !B@110 = no-current-target@210");
  }

  {
    /* eleven overloaded: ten tries, then all-targets-overloaded */
    fake_t many[] = {{'A', "v", 0, 1}, {'B', "v", 0, 1}, {'C', "v", 0, 1},
                     {'D', "v", 0, 1}, {'E', "v", 0, 1}, {'F', "v", 0, 1},
                     {'G', "v", 0, 1}, {'H', "v", 0, 1}, {'I', "v", 0, 1},
                     {'J', "v", 0, 1}, {'K', "v", 0, 1}};
    RUNS(many, 0,
         "A+0 !A@1 B+101 !B@102 C+202 !C@203 D+303 !D@304 E+404 !E@405 "
         "F+505 !F@506 G+606 !G@607 H+707 !H@708 I+808 !I@809 J+909 !J@910 "
         "= all-targets-overloaded@910");
  }

  {
    /* one target that is overloaded once: 1.6.2 gives up all the same */
    fake_t one[] = {{'A', "vo", 0, 5}};
    RUNS(one, 0, "A+0 !A@5 = no-current-target@105");
  }

  /* ------------------------------------------------------- patient */

  {
    /* overloaded once: one wait of 1100, the target is half-open, answers */
    fake_t one[] = {{'A', "vo", 0, 5}};
    RUNS(one, 1, "A+0 !A@5 A+1205 = answered@1210");
  }

  {
    /* always overloaded: the wait is spent once, then no-current-target */
    fake_t one[] = {{'A', "v", 0, 5}};
    RUNS(one, 1, "A+0 !A@5 A+1205 !A@1210 = no-current-target@1310");
  }

  {
    /**
     * A timeout uses up the budget: no retry after 5555 - where 1.6.2 would
     * wait 100 ms more to find nothing.
     */
    fake_t one[] = {{'A', "t", 0, 5}};
    RUNS(one, 1, "A+0 !A@5555 = all-targets-overloaded@5555");
  }

  {
    /* after the wait a try gets what is left of the budget, not 5555 */
    fake_t one[] = {{'A', "vt", 0, 5}};
    RUNS(one, 1, "A+0 !A@5 A+1205 !A@5555 = all-targets-overloaded@5555");
  }

  {
    /* with two targets nothing changes: the retry goes to the other one */
    fake_t two[] = {{'A', "v", 0, 5}, {'B', "o", 0, 5}};
    RUNS(two, 1, "A+0 !A@5 B+105 = answered@110");
  }

  /* ------------------------- the cases of fix/consume-wait's balance.test.js */

  {
    /**
     * Three targets that take 300 ms to say they are overloaded, and 750 ms
     * for the call: the second ends at 700, and a retry 100 ms later would
     * end past the budget - all-targets-overloaded, within it.
     */
    fake_t three[] = {{'A', "v", 0, 300}, {'B', "v", 0, 300}, {'C', "v", 0, 300}};
    RUNS_WITH(three, 750, 1000, 100, 5555,
              "A+0 !A@300 B+400 !B@700 = all-targets-overloaded@700");
  }

  {
    /**
     * Two targets that never answer, a request timeout of 400 ms and a
     * circuit breaker of 100/10 - half-open again by their next turn, which
     * was ten full timeouts in 1.6.2. One try uses up the budget.
     */
    fake_t two[] = {{'A', "t", 0, 5}, {'B', "t", 0, 5}};
    RUNS_WITH(two, 400, 100, 10, 400, "A+0 !A@400 = all-targets-overloaded@400");
  }

  {
    /* ... and 1.6.2 with the same numbers: ten of them */
    char got[2048];
    fake_t two[] = {{'A', "t", 0, 5}, {'B', "t", 0, 5}};
    bal_options_t options;

    bal_defaults(&options);
    options.patient = 0;
    options.closingTimeout = 100;
    options.retryTimeout = 10;
    runWith(two, 2, options, 400, got, sizeof got);
    CHECK(strstr(got, "= all-targets-overloaded@4090") != NULL, "1.6.2: %s", got);
  }

  {
    /* a wait that would end past the budget is no-current-target at once */
    fake_t one[] = {{'A', "vo", 0, 5}};
    RUNS_WITH(one, 1000, 1000, 100, 5555, "A+0 !A@5 = no-current-target@105");
  }

  {
    /* patient is no excuse for a first choice that fails: no wait */
    vg_t ring;
    bal_options_t options;
    bal_consume_t call;

    bal_defaults(&options);
    options.patient = 1;
    ring.init(options.closingTimeout, 0);
    bal_begin(&call, &options, 0);
    CHECK(bal_noTarget(&call, 0) == BAL_DONE &&
              call.outcome == BAL_NO_CURRENT_TARGET,
          "an empty ring waited");
    ring.release();
  }

  {
    /* the default is patient, with balance.js's numbers */
    bal_options_t options;

    bal_defaults(&options);
    CHECK(options.patient == 1 && options.tries == 10 &&
              options.retryTimeout == 100 && options.closingTimeout == 1000,
          "defaults are not balance.js's");
  }

  if (failures != 0) {
    fprintf(stderr, "balance: %d of %d checks failed\n", failures, checked);
    return 1;
  }

  printf("balance: %d checks ok\n", checked);
  return 0;
}
