/** The test echo service; echo.h has the commands. */
#include <stdint.h>
#include <stdio.h>

#include <meta_tasks.h>
#include <meta_text.h>

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

  long ms = args.ms.isNothing() ? 6000 : args.ms.number();

  meta_sleepMs(ms);

  json_t res = {code: 200, msg: "late"};

  return (mwm_answer_t){0, res};
}

/** ping after ms milliseconds: what concurrency is tested with. */
static mwm_answer_t delay(json_t args) {

  long ms = args.ms.isNothing() ? 0 : args.ms.number();

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

  long n = args.n.isNothing() ? BURN_ROUNDS : args.n.number();
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

  /* the first four never park and run in the reader; slow and delay sleep,
     burn holds its carrier for a millisecond: a task each */
  static const struct {
    const char *cmd;
    mwm_handler_t handler;
    int inline_;
  } commands[] = {{"ping", ping, 1},      {"fail", fail, 1},
                  {"throw", throwing, 1}, {"overload", overload, 1},
                  {"slow", slow, 0},      {"delay", delay, 0},
                  {"burn", burn, 0}};

  for (size_t i = 0; i < countof(commands); ++i) {

    char pattern[128];
    text_t written = TEXT`role:${role},cmd:${commands[i].cmd}`;

    written.into(pattern, sizeof pattern);

    if (commands[i].inline_)
      mwm_addInline(pattern, commands[i].handler);
    else
      mwm_add(pattern, commands[i].handler);
  }
}
