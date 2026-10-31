/**
 * loadgen: a closed-loop load generator for the microwizard ndjson wire, in
 * plain C, so that what is measured is the service and not the client.
 *
 *   loadgen HOST PORT CONNS THREADS INFLIGHT [seconds 5] [aE|a] [warmup 1]
 *           [server pid]
 *
 * CONNS connections over THREADS threads, an epoll of its own per thread.
 * Every connection keeps INFLIGHT calls outstanding: each answer sends the
 * next call, and all calls a wake makes on one connection go out in one
 * write. The call is cmd:add, as the benchmarks have it, in either form:
 *
 *   aE  {"id":"N","k":"aE","sync":true,"p":"cmd:add","d":{"a":A,"b":B}}
 *   a   {"id":"N","k":"a","sync":true,"args":{"cmd":"add","a":A,"b":B}}
 *
 * A and B follow from the id, so every answer is checked: its id must be one
 * outstanding on that connection and its res.res must be A + B. Anything
 * else counts as a failure, and a run with failures exits 1.
 *
 * Measured after the warmup, for the given seconds: calls answered per
 * second, the latency of each from its write to its answer (p50, p99, p999,
 * max) and the CPU seconds the generator itself spent - user and system,
 * also as a share of its threads, so it can be seen not to be the
 * bottleneck. With a server pid, its CPU seconds over the same window, per
 * call (/proc/PID/stat) - and its syscalls per call when it runs with a
 * counting LD_PRELOAD shim (scSample). With LG_SRV_CPUS / LG_GEN_CPUS, how
 * busy those cores were (/proc/stat): loopback's receive work is softirq on
 * the sender's core and charged to no process, so for a pinned server the
 * cores' busy time is the honest cost.
 *
 * One JSON line on stdout.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RING 8192 /* outstanding ids per connection, a power of two */
#define INBUF 65536
#define OUTBUF 65536

/* latency histogram: ns, 32 sub-buckets per power of two (~3% resolution) */
#define SUB 5
#define BUCKETS (64 << SUB)

typedef struct {
  int fd;
  uint64_t next;     /* the id of the next call */
  uint64_t sent[RING]; /* send time by id % RING, 0 when not outstanding */
  char in[INBUF];
  size_t have;
  char out[OUTBUF];
  size_t outLen;
} conn_t;

typedef struct {
  pthread_t thread;
  int index;
  conn_t *conns;
  int count;
  uint64_t hist[BUCKETS];
  uint64_t done;
  uint64_t failed;
  uint64_t maxNs;
} worker_t;

static const char *host;
static int port, inflight, argsForm;
static atomic_int phase; /* 0 warmup, 1 measuring, 2 stop */

static uint64_t nowNs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int bucketOf(uint64_t v) {
  if (v < (1u << SUB))
    return (int)v;
  int top = 63 - __builtin_clzll(v);
  int shift = top - SUB;
  return ((shift + 1) << SUB) + (int)((v >> shift) & ((1u << SUB) - 1));
}

static uint64_t valueOf(int b) {
  if (b < (1 << SUB))
    return (uint64_t)b;
  int shift = (b >> SUB) - 1;
  uint64_t sub = (uint64_t)(b & ((1 << SUB) - 1));
  return ((1ull << SUB) + sub) << shift;
}

static void die(const char *what) {
  perror(what);
  exit(2);
}

static void flush(conn_t *c);

static void queueCall(conn_t *c, uint64_t at) {
  uint64_t id = c->next++;
  long a = (long)(id % 1000), b = (long)((id / 1000) % 1000);
  int n;

  if (c->sent[id & (RING - 1)] != 0) {
    fprintf(stderr, "loadgen: ring full - more than %d calls outstanding\n", RING);
    exit(2);
  }
  c->sent[id & (RING - 1)] = at;

  /* a line is under 128 bytes; a burst bigger than the buffer goes out in parts */
  if (OUTBUF - c->outLen < 128)
    flush(c);

  if (argsForm)
    n = snprintf(c->out + c->outLen, OUTBUF - c->outLen,
                 "{\"id\":\"%llu\",\"k\":\"a\",\"sync\":true,\"args\":{\"cmd\":"
                 "\"add\",\"a\":%ld,\"b\":%ld}}\n",
                 (unsigned long long)id, a, b);
  else
    n = snprintf(c->out + c->outLen, OUTBUF - c->outLen,
                 "{\"id\":\"%llu\",\"k\":\"aE\",\"sync\":true,\"p\":\"cmd:add\","
                 "\"d\":{\"a\":%ld,\"b\":%ld}}\n",
                 (unsigned long long)id, a, b);
  c->outLen += (size_t)n;
}

static void flush(conn_t *c) {
  size_t off = 0;
  while (off < c->outLen) {
    ssize_t w = send(c->fd, c->out + off, c->outLen - off, MSG_NOSIGNAL);
    if (w > 0) {
      off += (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR)
      continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      /* the closed loop bounds what is outstanding; a full socket is rare */
      struct timespec ts = {0, 20000};
      nanosleep(&ts, NULL);
      continue;
    }
    die("send");
  }
  c->outLen = 0;
}

/* one answer line: {"id":"N",...,"res":{"res":S}} */
static int check(worker_t *w, conn_t *c, char *line, size_t len, uint64_t at) {
  char *p = memmem(line, len, "\"id\":\"", 6);
  char *end;
  uint64_t id, sent;
  long res;

  if (p == NULL)
    return -1;
  id = strtoull(p + 6, &end, 10);
  if (*end != '"')
    return -1;
  sent = c->sent[id & (RING - 1)];
  if (sent == 0 || id >= c->next)
    return -1;
  c->sent[id & (RING - 1)] = 0;

  p = memmem(line, len, "\"res\":{\"res\":", 13);
  if (p == NULL)
    return -1;
  res = strtol(p + 13, &end, 10);
  if (res != (long)(id % 1000) + (long)((id / 1000) % 1000))
    return -1;

  if (atomic_load_explicit(&phase, memory_order_relaxed) == 1) {
    uint64_t ns = at - sent;
    ++w->hist[bucketOf(ns)];
    ++w->done;
    if (ns > w->maxNs)
      w->maxNs = ns;
  }
  return 0;
}

static void *run(void *arg) {
  worker_t *w = arg;
  int ep = epoll_create1(0);
  struct epoll_event evs[256];

  if (ep < 0)
    die("epoll_create1");

  for (int i = 0; i < w->count; ++i) {
    conn_t *c = &w->conns[i];
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = c};
    uint64_t at = nowNs();
    for (int k = 0; k < inflight; ++k)
      queueCall(c, at);
    flush(c);
    if (epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ev) != 0)
      die("epoll_ctl");
  }

  while (atomic_load_explicit(&phase, memory_order_relaxed) != 2) {
    int n = epoll_wait(ep, evs, 256, 100);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      die("epoll_wait");

    for (int e = 0; e < n; ++e) {
      conn_t *c = evs[e].data.ptr;
      ssize_t got = recv(c->fd, c->in + c->have, INBUF - c->have, 0);
      uint64_t at;
      char *from, *nl;
      int answers = 0;

      if (got == 0) {
        fprintf(stderr, "loadgen: the server closed a connection\n");
        exit(2);
      }
      if (got < 0) {
        if (errno == EAGAIN || errno == EINTR)
          continue;
        die("recv");
      }
      c->have += (size_t)got;
      at = nowNs();

      from = c->in;
      while ((nl = memchr(from, '\n', c->in + c->have - from)) != NULL) {
        if (check(w, c, from, (size_t)(nl - from), at) != 0) {
          ++w->failed;
          if (w->failed <= 3)
            fprintf(stderr, "loadgen: bad answer: %.*s\n", (int)(nl - from), from);
        }
        ++answers;
        from = nl + 1;
      }
      c->have -= (size_t)(from - c->in);
      memmove(c->in, from, c->have);

      for (int k = 0; k < answers; ++k)
        queueCall(c, at);
      if (c->outLen)
        flush(c);
    }
  }
  return NULL;
}

static double cpuOf(int who) {
  struct rusage ru;
  getrusage(who, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec +
         ru.ru_stime.tv_usec / 1e6;
}

/* utime + stime of a process in seconds */
static int serverSample(int pid, double *cpu) {
  char path[64], buf[4096];
  FILE *f;
  snprintf(path, sizeof path, "/proc/%d/stat", pid);
  if ((f = fopen(path, "r")) == NULL)
    return -1;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  char *p = strrchr(buf, ')');
  unsigned long long ut = 0, st = 0;
  /* after ")": state(3) ... utime is field 14, stime 15 */
  sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st);
  *cpu = (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
  return 0;
}

/**
 * Syscall counts of the server, when it runs with a counting LD_PRELOAD shim
 * that keeps them in /dev/shm/sc.PID as SC_NAMES uint64s (libc wrappers only:
 * futexes inside pthread do not show).
 */
#define SC_N 10
static const char *scNames[SC_N] = {"read", "recv", "write", "writev", "send",
                                    "epoll_wait", "epoll_ctl", "yield", "nanosleep",
                                    "poll"};

static int scSample(int pid, uint64_t *into) {
  char path[64];
  FILE *f;
  snprintf(path, sizeof path, "/dev/shm/sc.%d", pid);
  if ((f = fopen(path, "r")) == NULL)
    return -1;
  size_t n = fread(into, sizeof *into, SC_N, f);
  fclose(f);
  return n == SC_N ? 0 : -1;
}

/**
 * Busy and softirq ticks over a CPU list ("0-3,6"), from /proc/stat: the
 * loopback's receive work runs in softirq and is charged to no process, so
 * the cores' own busy time is the honest cost of a pinned server.
 */
static int inList(const char *list, int cpu) {
  const char *p = list;
  while (p && *p) {
    int a = atoi(p), b = a;
    const char *dash = strchr(p, '-'), *comma = strchr(p, ',');
    if (dash && (!comma || dash < comma))
      b = atoi(dash + 1);
    if (cpu >= a && cpu <= b)
      return 1;
    p = comma ? comma + 1 : NULL;
  }
  return 0;
}

static void cpuTicks(const char *list, unsigned long long *busy,
                     unsigned long long *soft, unsigned long long *all) {
  FILE *f = fopen("/proc/stat", "r");
  char line[512];
  *busy = *soft = *all = 0;
  if (!f || !list)
    return;
  while (fgets(line, sizeof line, f)) {
    int cpu;
    unsigned long long v[10] = {0};
    if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu, &v[0], &v[1],
               &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 9 ||
        !inList(list, cpu))
      continue;
    unsigned long long t = 0;
    for (int i = 0; i < 8; ++i)
      t += v[i];
    *all += t;
    *busy += t - v[3] - v[4];
    *soft += v[6];
  }
  fclose(f);
}

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr, "usage: loadgen HOST PORT CONNS THREADS INFLIGHT [seconds] "
                    "[aE|a] [warmup] [server pid]\n");
    return 2;
  }
  host = argv[1];
  port = atoi(argv[2]);
  int conns = atoi(argv[3]);
  int threads = atoi(argv[4]);
  inflight = atoi(argv[5]);
  double seconds = argc > 6 ? atof(argv[6]) : 5;
  argsForm = argc > 7 && strcmp(argv[7], "a") == 0;
  double warmup = argc > 8 ? atof(argv[8]) : 1;
  int pid = argc > 9 ? atoi(argv[9]) : 0;

  if (threads > conns)
    threads = conns;
  if (inflight * 2 > RING) {
    fprintf(stderr, "loadgen: at most %d in flight\n", RING / 2);
    return 2;
  }

  conn_t *all = calloc((size_t)conns, sizeof *all);
  worker_t *ws = calloc((size_t)threads, sizeof *ws);
  struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  inet_pton(AF_INET, host, &sa.sin_addr);

  for (int i = 0; i < conns; ++i) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0)
      die("socket");
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0)
      die("connect");
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    all[i].fd = fd;
  }

  /* connections dealt out in contiguous runs */
  for (int t = 0, at = 0; t < threads; ++t) {
    int count = conns / threads + (t < conns % threads);
    ws[t].index = t;
    ws[t].conns = all + at;
    ws[t].count = count;
    at += count;
  }
  for (int t = 0; t < threads; ++t)
    pthread_create(&ws[t].thread, NULL, run, &ws[t]);

  struct timespec wt = {(time_t)warmup, (long)((warmup - (time_t)warmup) * 1e9)};
  nanosleep(&wt, NULL);

  double sCpu0 = 0, sCpu1 = 0;
  uint64_t sc0[SC_N], sc1[SC_N];
  int haveSc = pid && scSample(pid, sc0) == 0;
  if (pid)
    serverSample(pid, &sCpu0);
  const char *srvCpus = getenv("LG_SRV_CPUS"), *genCpus = getenv("LG_GEN_CPUS");
  unsigned long long sb0, ss0, sa0, gb0, gs0, ga0, sb1, ss1, sa1, gb1, gs1, ga1;
  cpuTicks(srvCpus, &sb0, &ss0, &sa0);
  cpuTicks(genCpus, &gb0, &gs0, &ga0);
  double cpu0 = cpuOf(RUSAGE_SELF);
  uint64_t t0 = nowNs();
  atomic_store(&phase, 1);

  struct timespec mt = {(time_t)seconds, (long)((seconds - (time_t)seconds) * 1e9)};
  nanosleep(&mt, NULL);

  atomic_store(&phase, 2);
  uint64_t t1 = nowNs();
  double cpu1 = cpuOf(RUSAGE_SELF);
  cpuTicks(srvCpus, &sb1, &ss1, &sa1);
  cpuTicks(genCpus, &gb1, &gs1, &ga1);
  if (pid)
    serverSample(pid, &sCpu1);
  if (haveSc)
    haveSc = scSample(pid, sc1) == 0;

  for (int t = 0; t < threads; ++t)
    pthread_join(ws[t].thread, NULL);

  uint64_t hist[BUCKETS] = {0}, done = 0, failed = 0, maxNs = 0;
  for (int t = 0; t < threads; ++t) {
    for (int b = 0; b < BUCKETS; ++b)
      hist[b] += ws[t].hist[b];
    done += ws[t].done;
    failed += ws[t].failed;
    if (ws[t].maxNs > maxNs)
      maxNs = ws[t].maxNs;
  }

  double wall = (double)(t1 - t0) / 1e9;
  double q[3] = {0.5, 0.99, 0.999}, qv[3] = {0};
  for (int i = 0; i < 3; ++i) {
    uint64_t want = (uint64_t)(q[i] * (double)done), seen = 0;
    for (int b = 0; b < BUCKETS; ++b) {
      seen += hist[b];
      if (seen > want) {
        qv[i] = (double)valueOf(b) / 1000.0;
        break;
      }
    }
  }

  double genCpu = cpu1 - cpu0;
  printf("{\"conns\":%d,\"threads\":%d,\"inflight\":%d,\"form\":\"%s\","
         "\"calls\":%llu,\"failed\":%llu,\"rps\":%.0f,\"p50us\":%.1f,"
         "\"p99us\":%.1f,\"p999us\":%.1f,\"maxus\":%.1f,\"gencpu\":%.2f,"
         "\"genutil\":%.2f",
         conns, threads, inflight, argsForm ? "a" : "aE",
         (unsigned long long)done, (unsigned long long)failed, done / wall, qv[0],
         qv[1], qv[2], maxNs / 1000.0, genCpu, genCpu / wall / threads);
  if (pid) {
    double sc = sCpu1 - sCpu0;
    printf(",\"srvcpu\":%.2f,\"srvcores\":%.2f,\"srvusPerCall\":%.2f",
           sc, sc / wall, done ? sc * 1e6 / done : 0);
  }
  if (srvCpus && sa1 > sa0)
    printf(",\"srvCoresBusy\":%.2f,\"srvSoftirq\":%.2f,\"srvCoreUsPerCall\":%.2f",
           (double)(sb1 - sb0) / (sa1 - sa0), (double)(ss1 - ss0) / (sa1 - sa0),
           done ? (double)(sb1 - sb0) / sysconf(_SC_CLK_TCK) * 1e6 / done : 0);
  if (genCpus && ga1 > ga0)
    printf(",\"genCoresBusy\":%.2f", (double)(gb1 - gb0) / (ga1 - ga0));
  if (haveSc && done) {
    printf(",\"syscallsPerCall\":{");
    for (int i = 0, first = 1; i < SC_N; ++i) {
      if (sc1[i] == sc0[i])
        continue;
      printf("%s\"%s\":%.3f", first ? "" : ",", scNames[i], (double)(sc1[i] - sc0[i]) / done);
      first = 0;
    }
    printf("}");
  }
  printf("}\n");
  return failed ? 1 : 0;
}
