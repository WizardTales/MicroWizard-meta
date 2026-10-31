/** The test echo service; echo.h has the commands. */
#include <stdint.h>
#include <stdio.h>

#include <meta_tasks.h>

#include "echo.h"
#include "service.h"

#define BURN_ROUNDS 500000

static mwm_answer_t ping(json_t args) {

  json_t res = {code: 200, msg: {echo: args, from: "meta"}};

  return (mwm_answer_t){0, res};
}

static mwm_answer_t fail(json_t args) {

  json_t res = {code: 409, err: "conflict on purpose"};

  return (mwm_answer_t){0, res};
}

static mwm_answer_t throwing(json_t args) {

  json_t error = {message: "thrown on purpose", code: "E_PURPOSE"};

  return (mwm_answer_t){1, error};
}

static mwm_answer_t overload(json_t args) {

  json_t error = {message: "overloaded",
                  details: {message: "retry_later_err_overload"}};

  return (mwm_answer_t){1, error};
}

static mwm_answer_t slow(json_t args) {

  long ms = args.get("ms").isNothing() ? 6000 : args.get("ms").number();

  meta_sleepMs(ms);

  json_t res = {code: 200, msg: "late"};

  return (mwm_answer_t){0, res};
}

/** ping after ms milliseconds: what concurrency is tested with. */
static mwm_answer_t delay(json_t args) {

  long ms = args.get("ms").isNothing() ? 0 : args.get("ms").number();

  if (ms > 0)
    meta_sleepMs(ms);

  json_t res = {code: 200, msg: {echo: args, from: "meta"}};

  return (mwm_answer_t){0, res};
}

/**
 * n rounds of xorshift64 (BURN_ROUNDS, about a millisecond on a fast core), on the
 * carrier the whole time: what the benchmark measures CPU-bound calls with.
 */
static mwm_answer_t burn(json_t args) {

  long n = args.get("n").isNothing() ? BURN_ROUNDS : args.get("n").number();
  uint64_t x = 0x9E3779B97F4A7C15ull;

  for (long i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }

  json_t res = {code: 200, msg: {echo: args, from: "meta", h: (double)(x >> 12)}};

  return (mwm_answer_t){0, res};
}

void mwm_echoServe(const char *role) {

  static const char *names[] = {"ping", "fail", "throw", "overload", "slow",
                                "delay", "burn"};
  mwm_handler_t handlers[] = {ping, fail, throwing, overload, slow, delay, burn};

  /* the first four never park and run in the reader; slow and delay sleep,
     burn holds its carrier for a millisecond: a task each */
  static const int inline_[] = {1, 1, 1, 1, 0, 0, 0};

  for (int i = 0; i < 7; ++i) {

    char pattern[128];

    snprintf(pattern, sizeof pattern, "role:%s,cmd:%s", role, names[i]);
    if (inline_[i])
      mwm_addInline(pattern, handlers[i]);
    else
      mwm_add(pattern, handlers[i]);
  }
}
