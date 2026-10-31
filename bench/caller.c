/**
 * bench-client: the remote suite of microservice-benchmarks
 * (suites/remote.js, "Call remote actions") with meta's client - src/client.c,
 * consume, visigoth, one ndjson connection per target - as the caller.
 *
 *   bench-client HOST PORT CONCURRENCY [seconds 5] [mode actE|act|actStr]
 *
 * No mesh: the target is handed to the client the way the mesh hands it one
 * (mwm_clientMember with the meta a microwizard listen announces), pin
 * cmd:add at HOST:PORT. Then remote.js's method: a batch of CONCURRENCY calls
 * at once, wait for all, the next batch - for the given time, after a warm-up
 * of a tenth of it. Calls per second is batches per second times CONCURRENCY,
 * which is what the README's tables show. Latency of a call is from the start
 * of its batch to its own answer.
 *
 *   actE    actE("cmd:add", {a:5,b:3})   MicroWizardActE
 *   act     act({cmd:"add",a:5,b:3})     MicroWizard1
 *   actStr  act("cmd:add", {a:5,b:3})    MicroWizard2
 *
 * One JSON line on stdout per run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <meta_json.h>
#include <meta_tasks.h>

#include "client.h"
#include "msgpack.h"

#define MAXLAT 8000000

static int mode; /* 0 actE, 1 act, 2 actStr */
static long *lat;
static long latCount;
static long failures;

static long nowNs(void) {

  struct timespec t;

  clock_gettime(CLOCK_MONOTONIC, &t);

  return t.tv_sec * 1000000000L + t.tv_nsec;
}

static int once(void) {

  mwm_settled_t got;
  int good;

  if (mode == 0) {
    json_t args = {a: 5, b: 3};
    got = mwm_actE("cmd:add", args);
    args.release();
  } else if (mode == 1) {
    json_t args = {cmd: "add", a: 5, b: 3};
    got = mwm_act(NULL, args);
    args.release();
  } else {
    json_t args = {a: 5, b: 3};
    got = mwm_act("cmd:add", args);
    args.release();
  }

  good = !got.rejected && (long)got.value.res == 8;
  got.value.release();

  return good;
}

/** One call of a batch: its outcome and its time in the slot it was given. */
static void one(long began, long *took, int *good) {

  *good = once();
  *took = nowNs() - began;
}

static int cmpLong(const void *a, const void *b) {

  long x = *(const long *)a;
  long y = *(const long *)b;

  return x < y ? -1 : x > y;
}

static void announce(const char *host, int port) {

  mp_buf_t meta = {0};

  meta.map(5);
  meta.cstr("config");
  meta.map(7);
  meta.cstr("host");
  meta.cstr(host);
  meta.cstr("port");
  meta.integer(port);
  meta.cstr("timeout");
  meta.integer(5555);
  meta.cstr("connectTimeout");
  meta.integer(355);
  meta.cstr("pin");
  meta.array(1);
  meta.cstr("cmd:add");
  meta.cstr("model");
  meta.cstr("consume");
  meta.cstr("type");
  meta.cstr("tcp");
  meta.cstr("instance");
  meta.cstr("benchtarget");
  meta.cstr("identifier$");
  meta.cstr("benchtarget~cmd:add~1");
  meta.cstr("tag$");
  meta.cstr("mc~mwmbench");
  meta.cstr("v$");
  meta.integer(0);

  mwm_clientMember(1, "benchtarget~cmd:add~1", meta.data, meta.len);
  meta.release();
}

/** Batches of n until `until`; with record, every latency kept. */
static long batches(int n, long until, int record, long *slots, int *goods) {

  long calls = 0;

  while (nowNs() < until) {

    long began = nowNs();

    task all {
      for (int i = 0; i < n; ++i)
        go all one(began, &slots[i], &goods[i]);
    }

    for (int i = 0; i < n; ++i) {
      if (!goods[i])
        ++failures;
      if (record && latCount < MAXLAT)
        lat[latCount++] = slots[i];
    }

    calls += n;
  }

  return calls;
}

int main(int argc, char **argv) {

  mwm_client_options_t options;

  if (argc < 4) {
    fprintf(stderr, "bench-client HOST PORT CONCURRENCY [seconds] [actE|act|actStr]\n");
    return 2;
  }

  const char *host = argv[1];
  int port = atoi(argv[2]);
  int n = atoi(argv[3]);
  double seconds = argc > 4 ? atof(argv[4]) : 5;
  const char *how = argc > 5 ? argv[5] : "actE";
  long *slots = (long *)calloc((size_t)n, sizeof(long));
  int *goods = (int *)calloc((size_t)n, sizeof(int));
  long began;
  long calls;
  double took;

  mode = strcmp(how, "act") == 0 ? 1 : strcmp(how, "actStr") == 0 ? 2 : 0;
  lat = (long *)malloc(MAXLAT * sizeof(long));

  mwm_clientDefaults(&options);
  snprintf(options.instance, sizeof options.instance, "benchclient");
  mwm_clientStart(&options);
  announce(host, port);

  /* the first call dials; it has to come back before anything is counted */
  for (int tries = 0; !once(); ++tries) {
    if (tries == 50) {
      fprintf(stderr, "bench-client: no answer from %s:%d\n", host, port);
      return 1;
    }
    meta_sleepMs(100);
  }

  batches(n, nowNs() + (long)(seconds * 1e8), 0, slots, goods);
  failures = 0;

  began = nowNs();
  calls = batches(n, began + (long)(seconds * 1e9), 1, slots, goods);
  took = (nowNs() - began) / 1e9;

  qsort(lat, (size_t)latCount, sizeof(long), cmpLong);

  printf("{\"suite\":\"remote\",\"client\":\"meta\",\"mode\":\"%s\",\"c\":%d,"
         "\"calls\":%ld,\"s\":%.3f,\"rps\":%.0f,\"failures\":%ld,"
         "\"p50us\":%.1f,\"p99us\":%.1f}\n",
         how, n, calls, took, calls / took, failures,
         latCount ? lat[latCount / 2] / 1e3 : 0.0,
         latCount ? lat[(long)(latCount * 0.99)] / 1e3 : 0.0);
  fflush(stdout);

  mwm_clientStop();

  return 0;
}
