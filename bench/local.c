/**
 * bench-local: the in-process suite of microservice-benchmarks
 * (suites/local.js, "Call local actions") with meta's local path - pattern
 * match and handler call, no socket. One call after the other, as
 * benchmarkify runs an async test: the next starts when the last settled.
 *
 *   bench-local [seconds per test, 5] [rounds, 1]
 *
 * The tests and what they mirror (same handler, same payload):
 *
 *   MetaAct           act(NULL, {cmd:"add",a:5,b:3})     MicroWizard
 *   MetaActStr        act("cmd:add", {a:5,b:3})           MicroWizardStr
 *   MetaActE          actE("cmd:add,a:5", {b:3})          MicroWizard2 /
 *                                                         MicroWizard2StrNoMix
 *   MetaActEMix       actE("cmd:add"+",a:"+a, {a:5,b:3})  MicroWizard2Str
 *   MetaRespond       a whole aE request line through mwm_respond: parse,
 *                     match, handler, answer line - what the service does
 *                     per line, without the socket
 *
 * The handler is added on {cmd:'add', a:'5'} as local.js adds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <meta_json.h>

#include "add.h"
#include "client.h"
#include "service.h"

static double nowS(void) {

  struct timespec t;

  clock_gettime(CLOCK_MONOTONIC, &t);

  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/**
 * Settled with a res. Not == 8: actE("cmd:add,a:5", {b:3}) hands the
 * handler {b:3}, in microwizard too (a + b is NaN there, 3 here).
 */
static int check(mwm_settled_t got) {

  int good = !got.rejected && !got.value.get("res").isNothing();

  got.value.release();

  return good;
}

static int runAct(void) {
  json_t args = {cmd: "add", a: 5, b: 3};
  int good = check(mwm_act(NULL, args));
  args.release();
  return good;
}

static int runActStr(void) {
  json_t args = {a: 5, b: 3};
  int good = check(mwm_act("cmd:add", args));
  args.release();
  return good;
}

static int runActE(void) {
  json_t args = {b: 3};
  int good = check(mwm_actE("cmd:add,a:5", args));
  args.release();
  return good;
}

static int runActEMix(void) {
  json_t args = {a: 5, b: 3};
  char pattern[64];
  snprintf(pattern, sizeof pattern, "cmd:add,a:%ld", args.get("a").number());
  int good = check(mwm_actE(pattern, args));
  args.release();
  return good;
}

static const char LINE[] =
    "{\"id\":\"x1y2z3a4b5c6\",\"k\":\"aE\",\"sync\":true,\"p\":\"cmd:add,a:5\","
    "\"d\":{\"a\":5,\"b\":3}}";

static int runRespond(void) {
  char *out = mwm_respond(LINE, sizeof LINE - 1);
  int good = out != NULL && strstr(out, "\"res\":{\"res\":") != NULL;
  free(out);
  return good;
}

typedef int (*test_t)(void);

int main(int argc, char **argv) {

  double seconds = argc > 1 ? atof(argv[1]) : 5;
  int rounds = argc > 2 ? atoi(argv[2]) : 1;
  static const char *names[] = {"MetaAct", "MetaActStr", "MetaActE",
                                "MetaActEMix", "MetaRespond"};
  test_t tests[] = {runAct, runActStr, runActE, runActEMix, runRespond};

  mwm_add("cmd:add,a:5", bench_add);

  for (int r = 0; r < rounds; ++r) {
    for (int i = 0; i < 5; ++i) {

      long count = 0;
      double began;
      double took;

      if (!tests[i]()) {
        fprintf(stderr, "%s: wrong answer\n", names[i]);
        return 1;
      }

      /* warm up a tenth of the time, then count, checking every 1000 */
      began = nowS();
      while (nowS() - began < seconds / 10)
        for (int k = 0; k < 1000; ++k)
          tests[i]();

      began = nowS();
      do {
        for (int k = 0; k < 1000; ++k)
          tests[i]();
        count += 1000;
      } while ((took = nowS() - began) < seconds);

      printf("{\"suite\":\"local\",\"test\":\"%s\",\"round\":%d,\"calls\":%ld,"
             "\"s\":%.3f,\"rps\":%.0f,\"avgNs\":%.1f}\n",
             names[i], r, count, took, count / took, took * 1e9 / count);
      fflush(stdout);
    }
  }

  return 0;
}
