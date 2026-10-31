/**
 * mwm-node: one member of a MicroWizard mesh, membership and calls.
 *
 *   mwm-node --pin 'role:cdn,cmd:*' [--pin 'role:cdn2,cmd:*'] [--listen-port 41000]
 *            [--echo cdn] [--events] [--client]
 *
 * It joins the bases over SWIM, announces itself the way a Node service with
 * `listen: [{pin, model: 'consume', type: 'tcp'}, ...]` does, keeps track of
 * everyone else with sneeze's rules, and answers the calls its pins draw:
 * ndjson over TCP, one reader task per connection, which runs a handler that
 * never parks itself and starts a task for every other call, the handlers in
 * service.c. Without handlers every call is answered the
 * way microwizard answers a pattern nobody serves - an answer without res.
 *
 * Every pin is a listen of its own, as every entry of `listen` is in
 * microwizard: its own SWIM member with its own UDP port, its own
 * identifier$ (instance~pin~Date.now()), its own incarnation, its own TCP
 * listener - one process with two pins is two members to everyone else,
 * with the same instance. All of them feed one client (client.c), which is
 * how this node calls others: every member's pin is a target.
 *
 * Configuration as rc('wzdc') reads it from the environment, which is how
 * every service in core is configured:
 *
 *   wzdc_seneca__bases   "ip:port,ip:port" (":port" and "ip" filled in as
 *                        mesh.js does: 127.0.0.1, 39999)
 *   wzdc_seneca__host    the address we announce and bind (127.0.0.1)
 *   wzdc_seneca__port    our SWIM port; a base defaults to 39999, everyone
 *                        else to a random one in [40000, 50000)
 *   wzdc_seneca__tag     the mesh tag; members are "mc~<tag>", default "mesh"
 *   wzdc_baseName        refused: it means a DNS lookup, which this does not
 *                        do (see docs/protocol.md)
 *
 * Command line, after the environment:
 *
 *   --pin P          a pattern we serve, one listen each; again for more
 *                    (null:true, like a pinless client, when not given)
 *   --listen-port N  the TCP port of the first listen, N+1 of the second ...
 *                    (0: the kernel picks, as it does for a Node service)
 *   --port N         the SWIM port of the first listen, N+1 of the second ...
 *                    (microwizard hands one fixed port to every listen,
 *                    which with two listens cannot bind; unset, each gets a
 *                    random one, as here)
 *   --echo ROLE      serve the test echo service as role:ROLE (src/echo.h);
 *                    again for more roles
 *   --isbase         a base: pin role:mesh,base:true, port 39999 by default
 *   --host H, --bases LIST, --tag T   as the environment above
 *   --events         one JSON line per mesh event on stdout, the client's
 *                    targets included
 *   --client         read calls from stdin, one JSON object per line, and
 *                    write each outcome as a line on stdout (below)
 *   --no-patient     consume exactly as microwizard 1.6.2 (balance.h);
 *                    --patient, the default, waits once for an opened
 *                    target and keeps a call within one request timeout
 *   --timeout MS     the client's request timeout (5555) for a member that
 *                    announces none, and what a patient call may take in all
 *   --keepalive MS   the client's keepalive interval (120000), for tests
 *   --max-inflight N calls running at once in this process, over all
 *                    connections (10000); a call beyond that is answered
 *                    retry_later_err_overload with details, which a
 *                    caller's balancer counts as overload and retries
 *                    elsewhere
 *   --any-address    lift the loopback fence (below)
 *
 * --client takes
 *
 *   {"tag": 1, "act": "role:echo,cmd:ping", "args": {"n": 1}}
 *   {"tag": 2, "args": {"role": "echo", "cmd": "ping"}}
 *   {"tag": 3, "actE": "role:echo,cmd:ping", "data": {"n": 1}}
 *
 * and answers {"ev":"result","tag":1,"ms":12,"result":...} (no result for
 * undefined) or {"ev":"error","tag":1,"ms":105,"error":{"err":...,"msg":...}}.
 *
 * SIGTERM and SIGINT go the way role:mc,cmd:close goes in microwizard: every
 * call from then on is answered retry_later_err_overload, the calls already
 * running are finished and answered, and half a second after the last of
 * them the process ends. A second signal ends it at once.
 *
 * The fence: by default it sends datagrams to 127.0.0.0/8 only and drops -
 * and reports - anything else, listens on 127.0.0.0/8 only, and its client
 * dials 127.0.0.0/8 only. It never resolves a name. --any-address lifts the
 * fence; SWIM and the ndjson calls have no authentication, so only use it
 * on a network where every peer is trusted.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include <meta_json.h>
#include <meta_net.h>
#include <meta_tasks.h>
#include <meta_tcp.h>

#include "client.h"
#include "echo.h"
#include "mesh.h"
#include "msgpack.h"
#include "service.h"
#include "swim.h"

/* tcp.js: a connection nobody has said anything on for 3 min is ended */
#define IDLE_MS (3L * 60 * 1000)
#define LONGEST (16 * 1024 * 1024)
/* what a reader takes in one turn before it lets its carrier's others run */
#define TURN_LINES 32
#define TURN_BYTES 65536
#define MAX_INFLIGHT 10000
#define GRACE_MS 500
#define DRAIN_MS 50

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 39999

#define LISTENS 8

typedef struct {
  char host[64];
  int port;
  char tag[64];
  char pins[LISTENS][256];
  int pinCount;
  int listenPort;
  int isbase;
  int events;
  int anyAddress;
  int client;
  int patient; /* -1: the client's default, which is patient */
  long timeout;
  long keepalive;
  long maxInflight;
  char echo[LISTENS][64];
  int echoCount;
  char *[] bases;
} config_t;

typedef struct node_t node_t;

/**
 * One entry of microwizard's `listen`: a SWIM member of its own - socket,
 * swim, sneeze table - and a TCP listener of its own.
 */
typedef struct {
  node_t *node;
  int index;
  const char *pin;
  int port;
  int fd;
  swim_t *swim;
  tcpListener_t *server;
  int listenPort;
  char identifier[MESH_ID_MAX];
  mesh_t mesh;
  long rejoinAt;
  long checkBasesAt;
  int joined;
} listen_t;

struct node_t {
  config_t config;
  char instance[16];
  char tagged[80];
  listen_t listens[LISTENS];
  int count;
  int fenced;

  /* SIGTERM and SIGINT, as descriptors (meta_signalFd) */
  int term;
  int interrupt;
};

static void say(const char *format, ...) {

  va_list args;
  char line[1024];
  struct timespec now;

  clock_gettime(CLOCK_REALTIME, &now);

  va_start(args, format);
  vsnprintf(line, sizeof line, format, args);
  va_end(args);

  fprintf(stderr, "%ld.%03ld mwm-node: %s\n", (long)now.tv_sec,
          now.tv_nsec / 1000000, line);
}

static long wallMs(void) {

  struct timespec now;

  clock_gettime(CLOCK_REALTIME, &now);

  return now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

/* --------------------------------------------------------------- config */

static void copyText(char *into, size_t room, const char *from) {
  snprintf(into, room, "%s", from != NULL ? from : "");
}

/** mesh.js's resolve_bases: "ip:port" stays, ":port" and "ip" are filled in. */
static void addBases(config_t *config, const char *list) {

  char *copy = strdup(list);
  char *save = NULL;

  for (char *item = strtok_r(copy, ",", &save); item != NULL;
       item = strtok_r(NULL, ",", &save)) {

    char base[128];
    const char *colon = strrchr(item, ':');

    while (*item == ' ')
      ++item;

    if (*item == 0)
      continue;

    if (item[0] == ':')
      snprintf(base, sizeof base, "%s%s", DEFAULT_HOST, item);
    else if (colon == NULL)
      snprintf(base, sizeof base, "%s:%d", item, DEFAULT_PORT);
    else
      snprintf(base, sizeof base, "%s", item);

    config->bases.push(strdup(base));
  }

  free(copy);
}

static int configure(config_t *config, int argc, char **argv) {

  const char *env;

  copyText(config->host, sizeof config->host, DEFAULT_HOST);
  copyText(config->tag, sizeof config->tag, "mesh");
  config->patient = -1;
  config->maxInflight = MAX_INFLIGHT;

  if ((env = getenv("wzdc_seneca__host")) != NULL && *env != 0)
    copyText(config->host, sizeof config->host, env);
  if ((env = getenv("wzdc_seneca__port")) != NULL && *env != 0)
    config->port = atoi(env);
  if ((env = getenv("wzdc_seneca__tag")) != NULL && *env != 0)
    copyText(config->tag, sizeof config->tag, env);
  if ((env = getenv("wzdc_seneca__bases")) != NULL && *env != 0)
    addBases(config, env);

  for (int i = 1; i < argc; ++i) {

    const char *arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;

    if (strcmp(arg, "--isbase") == 0)
      config->isbase = 1;
    else if (strcmp(arg, "--events") == 0)
      config->events = 1;
    else if (strcmp(arg, "--any-address") == 0)
      config->anyAddress = 1;
    else if (strcmp(arg, "--client") == 0)
      config->client = 1;
    else if (strcmp(arg, "--patient") == 0)
      config->patient = 1;
    else if (strcmp(arg, "--no-patient") == 0)
      config->patient = 0;
    else if (value == NULL) {
      say("%s wants a value", arg);
      return 0;
    } else if (strcmp(arg, "--pin") == 0) {
      if (config->pinCount == LISTENS) {
        say("no more than %d pins", LISTENS);
        return 0;
      }
      copyText(config->pins[config->pinCount++], sizeof config->pins[0],
               argv[++i]);
    } else if (strcmp(arg, "--echo") == 0) {
      if (config->echoCount == LISTENS) {
        say("no more than %d echo roles", LISTENS);
        return 0;
      }
      copyText(config->echo[config->echoCount++], sizeof config->echo[0],
               argv[++i]);
    } else if (strcmp(arg, "--listen-port") == 0)
      config->listenPort = atoi(argv[++i]);
    else if (strcmp(arg, "--port") == 0)
      config->port = atoi(argv[++i]);
    else if (strcmp(arg, "--timeout") == 0)
      config->timeout = atol(argv[++i]);
    else if (strcmp(arg, "--keepalive") == 0)
      config->keepalive = atol(argv[++i]);
    else if (strcmp(arg, "--max-inflight") == 0) {
      config->maxInflight = atol(argv[++i]);
      if (config->maxInflight < 1) {
        say("--max-inflight wants a number above 0");
        return 0;
      }
    } else if (strcmp(arg, "--host") == 0)
      copyText(config->host, sizeof config->host, argv[++i]);
    else if (strcmp(arg, "--tag") == 0)
      copyText(config->tag, sizeof config->tag, argv[++i]);
    else if (strcmp(arg, "--bases") == 0) {
      for (b in config->bases)
        free(*b);
      config->bases.release();
      addBases(config, argv[++i]);
    } else {
      say("what is %s?", arg);
      return 0;
    }
  }

  if (config->pinCount == 0)
    copyText(config->pins[config->pinCount++], sizeof config->pins[0],
             config->isbase ? "role:mesh,base:true" : "null:true");

  if ((env = getenv("wzdc_baseName")) != NULL && *env != 0 &&
      config->bases.count == 0) {
    say("wzdc_baseName %s means a DNS lookup, and this does not look anything "
        "up - give wzdc_seneca__bases",
        env);
    return 0;
  }

  if (config->bases.count == 0 && !config->isbase) {
    say("no bases: give wzdc_seneca__bases or --bases");
    return 0;
  }

  if (config->port != 0 && config->port + config->pinCount - 1 > 65535) {
    say("port %d and %d pins do not fit", config->port, config->pinCount);
    return 0;
  }

  return 1;
}

/* ------------------------------------------------------------- the fence */

static int loopback(const char *host) { return strncmp(host, "127.", 4) == 0; }

/** "ip:port" split the way swim-ng's Net.sendBuffer splits it. */
static int splitHost(const char *host, char *ip, size_t room, int *port) {

  const char *colon = strchr(host, ':');

  if (colon == NULL || (size_t)(colon - host) >= room)
    return 0;

  memcpy(ip, host, (size_t)(colon - host));
  ip[colon - host] = 0;
  *port = atoi(colon + 1);

  return *port > 0 && *port < 65536;
}

static void sendDatagram(void *context, const char *host, const uint8_t *data,
                         size_t size) {

  listen_t *listen = (listen_t *)context;
  node_t *node = listen->node;
  meta_address_t to;
  char ip[64];
  int port;

  if (!splitHost(host, ip, sizeof ip, &port))
    return;

  if (!node->config.anyAddress && !loopback(ip)) {
    if (node->fenced++ < 5)
      say("not sending to %s: outside 127.0.0.0/8 (--any-address lifts that)",
          host);
    return;
  }

  /* numeric only: meta_address refuses a name rather than looking it up */
  if (meta_address(&to, ip, port) != 0)
    return;

  meta_udpSend(listen->fd, &to, data, size);
}

/* ------------------------------------------------------- our member meta */

static void makeInstance(char *into) {

  static const char hex[] = "0123456789abcdef";
  unsigned char bytes[11];

  getrandom(bytes, sizeof bytes, 0);

  for (int i = 0; i < 11; ++i)
    into[i] = hex[bytes[i] & 15];

  into[11] = 0;
}

/**
 * A listen's meta, key for key as microwizard 1.5.0 announces one:
 *
 *   {config: {warn: {...}, host, port, timeout: 5555, connectTimeout: 355,
 *             pin: [pin], model: 'consume', type: 'tcp', ismesh: true},
 *    instance, identifier$, tag$, v$: 0}
 *
 * The client side copies config into its tcp client options, so timeout and
 * connectTimeout here are what a Node caller waits for us - they are not
 * decoration. warn rides along for the same reason it does in Node: nobody
 * took it out. instance is the process's, the same in every listen.
 */
static void writeMeta(node_t *node, listen_t *listen, mp_buf_t *out) {

  static const char *warns[] = {"unknown_message_id", "invalid_kind",
                                "invalid_origin",     "no_message_id",
                                "message_loop",       "own_message"};

  out->map(5);
  out->cstr("config");
  out->map(9);

  out->cstr("warn");
  out->map(6);
  for (int i = 0; i < 6; ++i) {
    out->cstr(warns[i]);
    out->boolean(1);
  }

  out->cstr("host");
  out->cstr(node->config.host);
  out->cstr("port");
  out->integer(listen->listenPort);
  out->cstr("timeout");
  out->integer(5555);
  out->cstr("connectTimeout");
  out->integer(355);
  out->cstr("pin");
  out->array(1);
  out->cstr(listen->pin);
  out->cstr("model");
  out->cstr("consume");
  out->cstr("type");
  out->cstr("tcp");
  out->cstr("ismesh");
  out->boolean(1);

  out->cstr("instance");
  out->cstr(node->instance);
  out->cstr("identifier$");
  out->cstr(listen->identifier);
  out->cstr("tag$");
  out->cstr(node->tagged);
  out->cstr("v$");
  out->integer(0);
}

/* ---------------------------------------------------- the mesh, as sneeze */

/** Text as a JSON string, for the event lines; the caller frees it. */
static char *jsonQuotedOf(const char *text) {

  size_t room = 3;
  char *out;
  char *at;

  for (const char *c = text; *c != 0; ++c)
    room += (unsigned char)*c < 0x20 || *c == '"' || *c == '\\' ? 6 : 1;

  out = (char *)malloc(room);

  if (out == NULL)
    return NULL;

  at = out;
  *at++ = '"';

  for (const char *c = text; *c != 0; ++c) {
    if (*c == '"' || *c == '\\') {
      *at++ = '\\';
      *at++ = *c;
    } else if ((unsigned char)*c < 0x20)
      at += sprintf(at, "\\u%04x", (unsigned char)*c);
    else
      *at++ = *c;
  }

  *at++ = '"';
  *at = 0;

  return out;
}

static void event(listen_t *listen, const char *what, const mesh_peer_t *peer,
                  json_t meta) {

  char *pin = NULL;
  json_t config = meta.config;

  pin = jsonQuotedOf(config.pin.at(0).text());

  if (pin == NULL)
    return;

  printf("{\"ev\":\"%s\",\"t\":%ld,\"listen\":%d,\"id\":\"%s\",\"swim\":\"%s\","
         "\"pin\":%s,\"tcp\":\"%s:%ld\",\"instance\":\"%s\","
         "\"incarnation\":%lld}\n",
         what, wallMs(), listen->index, peer->identifier, peer->host, pin,
         config.host.text(), config.port.number(),
         meta.instance.text(), (long long)peer->incarnation);
  fflush(stdout);
  free(pin);
}

/**
 * What a listen's sneeze table says: reported (as mesh.js's addClient and
 * removeClient would see it - not for members of our own instance, which
 * mesh.js passes over) and handed to the client, which makes targets of it.
 */
static void meshEmit(void *context, int added, const mesh_peer_t *peer) {

  listen_t *listen = (listen_t *)context;
  node_t *node = listen->node;
  json_t meta = mp_decode(peer->meta, peer->metaLen);
  defer meta.release();

  if (meta._refused != NULL ||
      strcmp(meta.instance, node->instance) == 0)
    return;

  if (node->config.events)
    event(listen, added ? "add" : "remove", peer, meta);

  mwm_clientMember(added, peer->identifier, peer->meta, peer->metaLen);
}

static void swimEvent(void *context, swim_event_t what, const swim_member_t *m) {

  listen_t *listen = (listen_t *)context;
  node_t *node = listen->node;
  long now = meta_monotonicMs();

  switch (what) {
  case SWIM_EVENT_JOINED:
    if (!listen->joined && node->config.events) {
      printf("{\"ev\":\"joined\",\"t\":%ld,\"listen\":%d}\n", wallMs(),
             listen->index);
      fflush(stdout);
    }
    listen->joined = 1;
    break;
  case SWIM_EVENT_JOIN_FAILED: {
    /* sneeze's retry wait, 111 to 555 ms - but for as long as it takes */
    unsigned int r = 0;

    getrandom(&r, sizeof r, 0);
    listen->rejoinAt = now + 111 + (long)(r % 444);
    break;
  }
  case SWIM_EVENT_UPDATE:
  case SWIM_EVENT_CHANGE:
    if (m != NULL && strcmp(m->host, swim_local(listen->swim)->host) != 0)
      mesh_update(&listen->mesh, m);
    break;
  }
}

/**
 * What mesh.js's rediscover does when bases go, made simpler and steadier:
 * a non-base with no base among its members syncs to the bases again, every
 * 1111 ms, until one answers. A base that came back at the same address
 * starts with an empty table and would not learn about us otherwise.
 */
static void checkBases(listen_t *listen, long now) {

  node_t *node = listen->node;
  int found = 0;

  if (node->config.isbase || now < listen->checkBasesAt)
    return;

  listen->checkBasesAt = now + 1111;

  for (b in node->config.bases)
    found += swim_get(listen->swim, *b) != NULL;

  if (found == 0 && listen->rejoinAt < 0)
    listen->rejoinAt = now;
}

static void rejoin(listen_t *listen, long now) {

  node_t *node = listen->node;
  const char **hosts;
  size_t at = 0;

  if (listen->rejoinAt < 0 || now < listen->rejoinAt)
    return;

  listen->rejoinAt = -1;
  hosts = (const char **)malloc((node->config.bases.count + 1) * sizeof *hosts);

  if (hosts == NULL)
    return;

  for (b in node->config.bases)
    hosts[at++] = *b;

  swim_join(listen->swim, hosts, (int)at, now);
  free(hosts);
}

/* ------------------------------------------------------------- the loop */

static long nearest(listen_t *listen, long next) {

  long due = swim_nextDeadline(listen->swim);

  if (due >= 0 && (next < 0 || due < next))
    next = due;

  if (listen->rejoinAt >= 0 && (next < 0 || listen->rejoinAt < next))
    next = listen->rejoinAt;

  if (!listen->node->config.isbase && (next < 0 || listen->checkBasesAt < next))
    next = listen->checkBasesAt;

  return next;
}

static void receive(listen_t *listen) {

  uint8_t datagram[65536];
  meta_address_t from;
  long got;

  while ((got = meta_udpReceive(listen->fd, datagram, sizeof datagram,
                                &from)) > 0) {

    char ip[64];
    char host[SWIM_HOST_MAX + 8];
    int port;

    if (meta_addressText(&from, ip, sizeof ip, &port) != 0)
      continue;

    snprintf(host, sizeof host, "%s:%d", ip, port);
    swim_receive(listen->swim, datagram, (size_t)got, host, meta_monotonicMs());
  }
}

/**
 * SIGTERM or SIGINT: role:mc,cmd:close - answer overload while the others
 * notice, and finish what is running. A second one ends the process now, as
 * the harness's stopOn does.
 */
static void goOffline(long *stopAt) {

  if (*stopAt >= 0) {
    say("stopping now");
    exit(0);
  }

  say("going offline");
  mwm_goOffline();
  *stopAt = meta_monotonicMs() + GRACE_MS;
}

/**
 * One wait covers the listens' datagrams, the signals and the next deadline.
 * The listens are as many as there are pins, which only the configuration
 * says, and the arms of a `select` are written out - so this is the call a
 * select over descriptors lowers to, meta_pollUntil, over all of them at
 * once: the two signals first, then every listen's socket. Each socket that
 * is readable is drained.
 */
static void swimRun(node_t *node, long *running) {

  struct pollfd fds[2 + LISTENS];
  size_t count = 2 + (size_t)node->count;
  long stopAt = -1;
  int draining = 0;

  memset(fds, 0, sizeof fds);
  fds[0].fd = node->term;
  fds[1].fd = node->interrupt;

  for (int i = 0; i < node->count; ++i)
    fds[2 + i].fd = node->listens[i].fd;

  for (size_t i = 0; i < count; ++i)
    fds[i].events = POLLIN;

  for (;;) {

    long now = meta_monotonicMs();
    long next = -1;

    /**
     * close in microwizard: while calls are running, look again (every
     * DRAIN_MS here, every 500 ms there); once none is, half a second more
     * for the last answers to go out. Only calls that reached a handler
     * count - an overload answer is not waited for, in index.js as here.
     */
    if (stopAt >= 0) {
      if (__atomic_load_n(running, __ATOMIC_ACQUIRE) > 0) {
        draining = 1;
        stopAt = now + DRAIN_MS;
      } else if (draining) {
        draining = 0;
        stopAt = now + GRACE_MS;
      }
    }

    if (stopAt >= 0 && now >= stopAt)
      break;

    for (int i = 0; i < node->count; ++i) {
      swim_advance(node->listens[i].swim, now);
      rejoin(&node->listens[i], now);
      checkBases(&node->listens[i], now);
      next = nearest(&node->listens[i], next);
    }

    if (stopAt >= 0 && (next < 0 || stopAt < next))
      next = stopAt;

    if (next < 0)
      next = now + 1000;

    /* -1 is the deadline (ETIMEDOUT) or a stop: either way, look again */
    if (meta_pollUntil(fds, count, next) <= 0)
      continue;

    if (fds[0].revents & POLLIN) {
      meta_signalTake(node->term);
      goOffline(&stopAt);
    }

    if (fds[1].revents & POLLIN) {
      meta_signalTake(node->interrupt);
      goOffline(&stopAt);
    }

    for (int i = 0; i < node->count; ++i)
      if (fds[2 + i].revents != 0)
        receive(&node->listens[i]);
  }

  say("stopping");

  /**
   * The end of the process, from here: the connections are parked in reads
   * with three minutes of idle time, and nothing they hold needs more than
   * the kernel to clean up. microwizard ends its listener and its
   * connections the same way, by closing them under whoever is reading.
   */
  for (int i = 0; i < node->count; ++i)
    node->listens[i].server.stop();

  exit(0);
}

/* ------------------------------------------------------------- the calls */

typedef @chan(int, 1) drained_t;

/**
 * One call, in a task of its own: the line in, the answer out. Writes on one
 * connection take turns, so answers from many calls never interleave; they
 * go back in the order they are ready, and the id puts them together.
 *
 * Two counts go down when it is done, both with atomics, since calls run on
 * every carrier: the process's (`running`, the gate) and the connection's
 * (`writing`: every call of it, and its reader as one more). Whoever takes
 * the connection's to zero is the last to write; when that is a call, it
 * tells the reader, which is waiting to close.
 */
static void handle(tcp_t *peer, char *text, long *running, long *writing,
                   drained_t *drained) {

  char *out = mwm_respond(text, strlen(text));

  free(text);

  if (out != NULL) {
    peer.writeLine(out);
    free(out);
  }

  __atomic_sub_fetch(running, 1, __ATOMIC_ACQ_REL);

  /* the last thing touched: once it is zero the reader may be gone */
  if (__atomic_sub_fetch(writing, 1, __ATOMIC_ACQ_REL) == 0)
    drained->send(1);
}

/**
 * The answers a reader has made and not written yet: the lines already in
 * its read buffer are answered before any of them go, a turn's worth at most
 * (serve), and then all of them go in one write - a wake that brings
 * thirty-two calls costs one write, not thirty-two. Lines from tasks
 * (handle) still write their own.
 */
typedef struct {
  char *text;
  size_t length;
  size_t room;
} pending_t;

static void pendingAdd(pending_t *pending, char *out) {

  size_t length = strlen(out);

  if (pending->length + length + 1 > pending->room) {

    size_t room = pending->room != 0 ? pending->room : 4096;

    while (room < pending->length + length + 1)
      room *= 2;

    pending->text = (char *)realloc(pending->text, room);
    pending->room = room;
  }

  memcpy(pending->text + pending->length, out, length);
  pending->text[pending->length + length] = '\n';
  pending->length += length + 1;
}

static void pendingFlush(tcp_t *peer, pending_t *pending) {

  if (pending->length == 0)
    return;

  peer.write(pending->text, pending->length);
  pending->length = 0;
}

/** Whether the read buffer holds another whole line: no read needed for it. */
static int lineWaiting(tcp_t *peer) {

  return peer->_end > peer->_start &&
         memchr(peer->_in + peer->_start, '\n', peer->_end - peer->_start) != NULL;
}

/** An answer that does not need a task, made by the reader: written with
    the others before the reader next has to wait for a line (serve). */
static void answerNow(pending_t *pending, char *out) {

  if (out != NULL) {
    pendingAdd(pending, out);
    free(out);
  }
}

/**
 * One connection: one reader, and a task per call whose handler may park,
 * as a Node service has a promise per call; a handler added with
 * mwm_addInline never parks, and the reader runs it itself, as the event
 * loop runs one in Node. The process's gate bounds the calls running at once;
 * a call beyond it, and every call while going offline, is answered by the
 * reader at once and runs nothing. A line that is not JSON ends the
 * connection, as the ndjson parser's error does in tcp.js; so does three
 * minutes of silence. The calls are started into the scope handed down,
 * not a task block of our own, which would hold a carrier per connection;
 * the connection is closed once the last of them has written.
 */
static void serve(@scope all, tcp_t *peer, long *running, long max) {

  long writing = 1;
  drained_t drained;
  pending_t pending = {NULL, 0, 0};
  long turn = 0;

  peer.idle(IDLE_MS);

  for (;;) {

    const char *text;
    size_t length;
    mwm_inline_t reading;

    /**
     * The answers made so far go before a read that may wait. And a turn is
     * TURN_LINES lines or TURN_BYTES of answers: then they go, and the reader
     * goes to the back of its carrier's queue. Answering everything in the
     * buffer before parking again held the carrier's other readers for all
     * of it - 256 connections with sixteen calls each on one carrier had a
     * p99 of 100 ms. Only with a whole line still waiting, so the yield
     * always comes back to work and never to a read.
     */
    if (!lineWaiting(peer)) {
      pendingFlush(peer, &pending);
      turn = 0;
    } else if (turn >= TURN_LINES || pending.length >= TURN_BYTES) {
      pendingFlush(peer, &pending);
      meta_yield();
      turn = 0;
    }

    text = peer.line(LONGEST);

    if (text == NULL)
      break;

    ++turn;
    length = peer.length();

    if (length == 0)
      continue;

    /**
     * Answered here when that needs no task: a health ping, every call
     * while offline, a handler that never parks (mwm_addInline) - as a
     * Node service answers in its event loop. A line that is not JSON ends
     * the connection, as the ndjson parser's error does in tcp.js.
     */
    reading = mwm_respondInline(text, length, running, max);

    if (reading.reading == MWM_BROKEN)
      break;

    if (reading.reading == MWM_ANSWERED) {
      answerNow(&pending, reading.out);
      continue;
    }

    if (__atomic_add_fetch(running, 1, __ATOMIC_ACQ_REL) > max) {
      __atomic_sub_fetch(running, 1, __ATOMIC_ACQ_REL);
      answerNow(&pending, mwm_refuse(text, length));
      continue;
    }

    __atomic_add_fetch(&writing, 1, __ATOMIC_ACQ_REL);
    go all handle(peer, strdup(text), running, &writing, &drained);
  }

  /* what was answered before the end or a broken line still goes */
  pendingFlush(peer, &pending);
  free(pending.text);

  if (__atomic_sub_fetch(&writing, 1, __ATOMIC_ACQ_REL) != 0) {

    int one;

    drained.receive(&one);
  }

  peer.shutdown();
  peer.close();
}

/** Accepts until the listener is stopped. */
static void acceptAll(@scope all, tcpListener_t *server, long *running,
                      long max) {

  for (;;) {

    tcp_t *peer = server.accept();

    if (peer.failed()) {
      peer.close();
      return;
    }

    go all serve(all, peer, running, max);
  }
}

/* ---------------------------------------------------- calls from stdin */

/** One outcome line: the call's tag, how long it took, what it settled with. */
static void tell(json_t asked, long ms, int rejected, json_t got) {

  char *tag = asked.tag.isNothing() ? strdup("null")
                                           : yyjson_mut_val_write(asked.tag._node, 0, NULL);
  char *value = got.isNothing() ? NULL : yyjson_mut_val_write(got._node, 0, NULL);
  size_t room = strlen(tag) + (value != NULL ? strlen(value) : 0) + 128;
  char *line = (char *)malloc(room);

  if (line != NULL) {

    if (rejected)
      snprintf(line, room, "{\"ev\":\"error\",\"tag\":%s,\"ms\":%ld,\"error\":%s}\n",
               tag, ms, value != NULL ? value : "null");
    else if (value != NULL)
      snprintf(line, room, "{\"ev\":\"result\",\"tag\":%s,\"ms\":%ld,\"result\":%s}\n",
               tag, ms, value);
    else
      snprintf(line, room, "{\"ev\":\"result\",\"tag\":%s,\"ms\":%ld}\n", tag, ms);

    /* one write per line: the callers' lines do not interleave */
    fputs(line, stdout);
    fflush(stdout);
    free(line);
  }

  free(value);
  free(tag);
}

/** One call from stdin, in a task of its own: made, and its outcome told. */
static void caller(char *text) {

  json_t asked = meta_toJSON(text);
  defer asked.release();
  long began = meta_monotonicMs();
  mwm_settled_t got;

  free(text);

  if (asked._refused != NULL) {
    say("--client: not JSON: %s", asked._refused);
    return;
  }

  if (!asked.actE.isNothing())
    got = mwm_actE(asked.actE.text(), asked.data);
  else
    got = mwm_act(asked.act.isNothing() ? NULL : asked.act.text(),
                  asked.args);

  tell(asked, meta_monotonicMs() - began, got.rejected, got.value);
  got.value.release();
}

/**
 * Reads stdin a line at a time and makes each line a call of its own: a call
 * parks its task, and a task's record goes when the task does, so a task per
 * call costs nothing once it is over.
 */
static void readCalls(@scope all) {

  char buffer[65536];
  size_t have = 0;

  fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

  for (;;) {

    ssize_t got = read(0, buffer + have, sizeof buffer - have - 1);
    char *start = buffer;
    char *end;

    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      @readable(0);
      continue;
    }

    if (got <= 0)
      break;

    have += (size_t)got;
    buffer[have] = 0;

    while ((end = memchr(start, '\n', have - (size_t)(start - buffer))) != NULL) {
      *end = 0;
      if (end > start)
        go all caller(strdup(start));
      start = end + 1;
    }

    have -= (size_t)(start - buffer);
    memmove(buffer, start, have);

    if (have == sizeof buffer - 1) {
      say("--client: a line longer than %zu bytes, dropped", sizeof buffer);
      have = 0;
    }
  }
}

/* ------------------------------------------------------------- start */

/**
 * A listen comes up: its UDP socket, its TCP listener, its meta and its
 * swim. The identifier$ is mesh.js's - instance~pin~Date.now() - and the
 * incarnation a Date.now() of its own a moment later, as sneeze takes it,
 * which is what lets a restart on the same host:port win over the faulty
 * record everyone keeps of the run before.
 */
static int openListen(node_t *node, int index) {

  listen_t *listen = &node->listens[index];
  mp_buf_t meta = {0};
  swim_options_t options;
  swim_hooks_t hooks;
  char local[SWIM_HOST_MAX + 8];
  tcpListener_t *server;
  long started = wallMs();

  listen->node = node;
  listen->index = index;
  listen->pin = node->config.pins[index];
  listen->rejoinAt = -1;

  if (node->config.port != 0)
    listen->port = node->config.port + index;
  else if (node->config.isbase && index == 0)
    listen->port = DEFAULT_PORT;
  else {
    unsigned int r = 0;

    getrandom(&r, sizeof r, 0);
    listen->port = 40000 + (int)(r % 10000);
  }

  snprintf(listen->identifier, sizeof listen->identifier, "%s~%s~%ld",
           node->instance, listen->pin, started);
  snprintf(local, sizeof local, "%s:%d", node->config.host, listen->port);

  listen->fd = meta_udpOpen(node->config.host, listen->port, 0);

  if (listen->fd < 0) {
    say("cannot bind %s: %s", local, strerror(errno));
    return 0;
  }

  server = meta_tcpListen(node->config.host,
                          node->config.listenPort != 0
                              ? node->config.listenPort + index
                              : 0);

  if (server.failed()) {
    say("cannot listen on %s port %d: %s", node->config.host,
        node->config.listenPort != 0 ? node->config.listenPort + index : 0,
        server.error());
    server.close();
    return 0;
  }

  /* what is announced is what the kernel gave us */
  listen->server = server;
  listen->listenPort = server.port();

  mesh_init(&listen->mesh, node->tagged, listen->identifier, meshEmit, listen);
  writeMeta(node, listen, &meta);

  swim_defaults(&options);
  hooks.context = listen;
  hooks.send = sendDatagram;
  hooks.event = swimEvent;

  /* sneeze takes its incarnation from Date.now() too, a moment after the id */
  listen->swim = swim_new(&options, local, meta.data, meta.len, wallMs(), hooks);
  meta.release();

  if (listen->swim == NULL) {
    say("no memory");
    return 0;
  }

  say("%s as %s, pin %s, tag %s, tcp %d", local, listen->identifier,
      listen->pin, node->tagged, listen->listenPort);

  if (node->config.events) {
    printf("{\"ev\":\"started\",\"t\":%ld,\"listen\":%d,\"swim\":\"%s\","
           "\"id\":\"%s\",\"instance\":\"%s\",\"pin\":\"%s\",\"tcp\":\"%s:%d\"}\n",
           wallMs(), index, local, listen->identifier, node->instance,
           listen->pin, node->config.host, listen->listenPort);
    fflush(stdout);
  }

  swim_start(listen->swim, meta_monotonicMs());

  /* a base leaves itself out of the list, and joins alone if that empties it */
  listen->rejoinAt = meta_monotonicMs();
  listen->checkBasesAt = meta_monotonicMs() + 1111;

  return 1;
}

int main(int argc, char **argv) {

  node_t node;
  long started = wallMs();
  mwm_client_options_t client;
  const char *ownPins[LISTENS];

  /* the calls running, over every connection (serve) - read by swimRun */
  long running = 0;

  memset(&node, 0, sizeof node);

  /**
   * SIGTERM and SIGINT are the SWIM task's to read, as descriptors. Taken
   * first, so that one sent while the listens come up is not the default's
   * to end the process with.
   */
  node.term = meta_signalFd(SIGTERM);
  node.interrupt = meta_signalFd(SIGINT);

  if (node.term < 0 || node.interrupt < 0) {
    say("no descriptor for SIGTERM or SIGINT: %s", strerror(errno));
    return 1;
  }

  if (!configure(&node.config, argc, argv))
    return 2;

  if (!node.config.anyAddress) {

    if (!loopback(node.config.host)) {
      say("host %s is outside 127.0.0.0/8 (--any-address lifts that)",
          node.config.host);
      return 2;
    }

    for (b in node.config.bases) {
      if (!loopback(*b)) {
        say("base %s is outside 127.0.0.0/8 (--any-address lifts that)", *b);
        return 2;
      }
    }
  }

  srand((unsigned int)(started ^ getpid()));
  makeInstance(node.instance);
  snprintf(node.tagged, sizeof node.tagged, "mc~%s", node.config.tag);

  for (int i = 0; i < node.config.echoCount; ++i)
    mwm_echoServe(node.config.echo[i]);

  /* the client first: the first member a listen hears of is a target */
  mwm_clientDefaults(&client);
  copyText(client.instance, sizeof client.instance, node.instance);
  client.anyAddress = node.config.anyAddress;
  client.events = node.config.events;
  if (node.config.patient >= 0)
    client.balance.patient = node.config.patient;

  if (node.config.timeout > 0)
    client.timeout = node.config.timeout;

  if (node.config.keepalive > 0)
    client.keepaliveMs = node.config.keepalive;

  for (int i = 0; i < node.config.pinCount; ++i)
    ownPins[i] = node.config.pins[i];

  client.ownPins = ownPins;
  client.ownPinCount = node.config.pinCount;
  mwm_clientStart(&client);

  for (int i = 0; i < node.config.pinCount; ++i) {
    if (!openListen(&node, i))
      return 1;
    node.count = i + 1;
  }

  /* swimRun ends the process; this block is never left another way */
  task all {
    for (int i = 0; i < node.count; ++i)
      go all acceptAll(all, node.listens[i].server, &running, node.config.maxInflight);

    go all swimRun(&node, &running);

    if (node.config.client)
      go all readCalls(all);
  }

  mwm_clientStop();

  for (int i = 0; i < node.count; ++i) {
    node.listens[i].server.close();
    mesh_free(&node.listens[i].mesh);
    swim_free(node.listens[i].swim);
    meta_udpClose(node.listens[i].fd);
  }

  for (b in node.config.bases)
    free(*b);

  node.config.bases.release();

  return 0;
}
