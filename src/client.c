/**
 * The calling half of microwizard: mesh.js's clients, balance.js's consume,
 * tcp.js's client connections and transfer.js's handleResponse. client.h
 * says what; the comments here say where the JS is particular and where the
 * shape is the runtime's.
 *
 * The shape, once:
 *
 *   caller task --takes the router--> choose, then gives it back
 *        |                                (targets, visigoths, pins)
 *        +--REG--> the connection's reader    <-- answer lines
 *        +--its own line onto the connection
 *        +--WROTE--> the reader
 *        |
 *   caller task <------- one reply, from the reader --+
 *
 * The router is everything that changes with the mesh: the pin tree, the
 * groups and their visigoths, the targets. Nobody shares it - it is owned by
 * whoever took it off a channel of capacity one, and given back by sending it
 * there again, so it moves from task to task the way any value sent on a
 * channel does. A caller holds it for the match and the choice, the mesh for
 * a member, a reader for a change of its connection's state, the ticker for
 * calls that wait for a connection. Nothing parks while holding it.
 *
 * A connection is its reader's: one task that dials it, reads its lines and
 * owns the table of ids waiting on it - transfer.js's callmap, per
 * connection. A caller tells the reader its id (REG) before it writes its
 * line, so the answer can never be faster than the entry, writes the line
 * itself - meta_tcp hands writers the connection one whole line at a time -
 * and says WROTE when the line is out, so the reader closes the connection
 * only once nobody is writing on it. The reader hands every answer straight
 * to the caller's own reply channel: one handoff per call.
 *
 * Calls that come while there is no connection yet are the router's: they
 * wait in their target with their line until the reader has one, which
 * takes them over; the ticker times them out and dials again, as tcp.js's
 * reconnect does. A call runs in its caller's task from the first choice to
 * the last retry.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>

#include <meta_json.h>
#include <meta_tasks.h>
#include <meta_tcp.h>
#include <meta_time.h>

#include "client.h"
#include "msgpack.h"
#include "service.h"
#include "visigoth.h"

#define LONGEST (16 * 1024 * 1024)

/* transfer.js's dropReply: one line per kind this often */
#define DROP_LOG_MS 10000
#define DROP_KINDS 8

/* how soon a connection that could not be dialed is dialed again */
#define REDIAL_MS 100

/* what a reader reads at once, to begin with */
#define READ_CHUNK 65536

/* ------------------------------------------------------------ messages */

typedef enum {
  RE_WAIT,      /* not a reply: the request is out, the reply will come */
  RE_ANSWER,    /* got: the answer, parsed */
  RE_TIMEOUT,   /* no connection or no answer in time */
  RE_NO_PIN,    /* no pin matches: undefined, as microwizard resolves */
  RE_LOCAL,     /* a handler of our own wins: handler, run by the caller */
  RE_NO_TARGET, /* visigoth: no upstreams available */
  RE_SPENT,     /* chosen, but the call's budget is spent: no try */
  RE_CLOSED     /* the client is stopping */
} reply_kind_t;

typedef struct {
  reply_kind_t kind;
  json_t got;   /* RE_ANSWER, owned */
  char *target; /* RE_ANSWER, RE_TIMEOUT: the target's id, owned */
  mwm_handler_t handler; /* RE_LOCAL */
} reply_t;

typedef @chan(reply_t, 1) replies_t;

typedef enum {
  RG_CALL,  /* an id to wait for: it is about to be written */
  RG_WROTE  /* a caller that registered is done with the connection */
} reg_kind_t;

typedef struct {
  reg_kind_t kind;
  char *id;       /* RG_CALL, owned */
  long until;     /* RG_CALL: when it is a timeout */
  replies_t *back;
} reg_t;

typedef @chan(reg_t, 4096) regs_t;

typedef struct group_t group_t;
typedef struct target_t target_t;
typedef struct state_t state_t;

/**
 * A connection, on its reader's stack. The router finds it through its
 * target while it is up; line and regs are what callers use, the rest is
 * the router's and touched only by whoever holds it.
 */
typedef struct {
  tcp_t *line;
  regs_t *regs; /* the reader's */

  /* the router's */
  target_t *target; /* NULL once let go */
  long users;       /* callers that took it, each says WROTE once */
} conn_t;

/** A call that waits for its target to have a connection. */
typedef struct {
  char *id;
  char *line;
  long timeout; /* the answer's, once sent */
  long until;   /* the connection's */
  replies_t *back;
} unsent_t;

static @chan(state_t *, 1) router;

task clientScope;

/* set once by mwm_clientStart, before any task of ours runs; read after */
static bal_options_t balanceOptions;
static int started;

/* ---------------------------------------------------------------- state */

struct target_t {
  char *id;
  char *identifier;
  char *pin;
  char host[64];
  int port;
  long timeout;
  long connectTimeout;

  group_t *group;   /* NULL once retired */
  vg_node_t *node;  /* NULL when the ring was full, as balance.js's e */

  /* its connection: up, being dialed, or neither */
  conn_t *conn;
  int dialing;
  long lastDial;

  /* calls waiting for it, oldest first */
  unsent_t *[] unsent;
};

/** Targets in the order they came, which is the ring's order too. */
typedef target_t *[] targets_t;

/**
 * What a node of the pin tree holds: a handler of ours or a group of
 * targets. Ours and the mesh's are in one tree, as index.js adds a client
 * with `this.add(p, func)` beside the handlers.
 */
typedef struct {
  mwm_handler_t handler;
  group_t *group;
} holder_t;

struct group_t {
  char *key;  /* pattern(pin): the pairs sorted, as balance.js keys it */
  char *pin;  /* as first announced */
  holder_t *holder; /* its value in the pin tree, once it is there */
  vg_t ring;
  targets_t targets;
};

struct state_t {
  mwm_client_options_t options;
  char **ownPins;
  int ownPinCount;

  group_t *[] groups;      /* oldest first */
  targets_t retired;

  /* the pin tree, what its nodes hold, and how many handlers it has seen */
  mwm_pins_t *pins;
  holder_t *[] holders;
  int handlersSeen;

  int stopping;

  const char *dropKind[DROP_KINDS];
  long dropAt[DROP_KINDS];
  long dropped[DROP_KINDS];
};

/** A call a reader waits for. */
typedef struct {
  char *id;
  long until;
  replies_t *back;
} pending_t;

/**
 * The router, off its channel: NULL once the client is gone. A caller that
 * finds it taken tries a few more times before it parks, since whoever has
 * it gives it back within a microsecond or two.
 */
static state_t *take(void) {

  state_t *st = NULL;

  for (int i = 0; i < 32 && st == NULL; ++i) {
    select {
    case got in router:
      st = got;
    default:
      st = NULL;
    }
  }

  if (st == NULL && !router.receive(&st))
    return NULL;

  return st;
}

static void give(state_t *st) { router.send(st); }
/* ---------------------------------------------------------------- bits */

static long wallMs(void) {

  struct timespec now;

  clock_gettime(CLOCK_REALTIME, &now);

  return now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

static void say(const char *what, const char *detail) {
  fprintf(stderr, "%ld.%03ld mwm-client: %s %s\n", wallMs() / 1000,
          wallMs() % 1000, what, detail != NULL ? detail : "");
}

/** The uid package's shape: 11 hex characters. */
static void makeId(char *into) {

  static const char hex[] = "0123456789abcdef";
  unsigned char bytes[11];

  if (getrandom(bytes, sizeof bytes, 0) != (ssize_t)sizeof bytes)
    memset(bytes, 7, sizeof bytes);

  for (int i = 0; i < 11; ++i)
    into[i] = hex[bytes[i] & 15];

  into[11] = 0;
}

static char *textOf(const char *from) { return strdup(from != NULL ? from : ""); }

static int byText(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/**
 * mesh.js's makePinConfig and utilPattern: the member's config without pin
 * and pins, the canonical pin put back, every primitive field without a $
 * in its name as key:value, sorted, joined with commas - then '~' and the
 * identifier$. That is the target's id, which is what keeps a member that
 * is announced twice from being a target twice.
 */
static char *targetId(yyjson_mut_val *config, const char *pin,
                      const char *identifier) {

  char *parts[64];
  int count = 0;
  size_t room = strlen(identifier) + 2;
  char *out;
  char *at;
  yyjson_mut_obj_iter it;
  yyjson_mut_val *key;

  yyjson_mut_obj_iter_init(config, &it);

  while ((key = yyjson_mut_obj_iter_next(&it)) != NULL && count < 63) {

    const char *name = yyjson_mut_get_str(key);
    char *value;
    char part[600];

    if (strcmp(name, "pin") == 0 || strcmp(name, "pins") == 0 ||
        strchr(name, '$') != NULL)
      continue;

    /* String(): a primitive as `${value}` writes it */
    if ((value = mwm_jsText(yyjson_mut_obj_iter_get_val(key))) == NULL)
      continue;

    snprintf(part, sizeof part, "%s:%s", name, value);
    free(value);
    parts[count++] = strdup(part);
  }

  {
    char part[600];

    snprintf(part, sizeof part, "pin:%s", pin);
    parts[count++] = strdup(part);
  }

  qsort(parts, (size_t)count, sizeof parts[0], byText);

  for (int i = 0; i < count; ++i)
    room += strlen(parts[i]) + 1;

  out = (char *)malloc(room);
  at = out;

  for (int i = 0; i < count; ++i) {
    at += sprintf(at, "%s%s", i != 0 ? "," : "", parts[i]);
    free(parts[i]);
  }

  sprintf(at, "~%s", identifier);

  return out;
}

/** The fence: an address literal, and 127.0.0.0/8 unless it was lifted. */
static int dialable(const state_t *st, const char *host) {

  unsigned char address[16];

  if (inet_pton(AF_INET, host, address) == 1)
    return st->options.anyAddress || address[0] == 127;

  if (inet_pton(AF_INET6, host, address) == 1)
    return st->options.anyAddress;

  return 0; /* a name: never looked up */
}

static void event(const state_t *st, const char *what, const target_t *t) {

  if (!st->options.events)
    return;

  printf("{\"ev\":\"%s\",\"t\":%ld,\"pin\":\"%s\",\"id\":\"%s\",\"tcp\":\"%s:%d\"}\n",
         what, wallMs(), t->pin, t->id, t->host, t->port);
  fflush(stdout);
}

/**
 * transfer.js's dropReply: say so once per kind every ten seconds, with how
 * many went unsaid in between, so a peer that keeps sending them cannot
 * flood the log. Whatever the peer sends, the connection stays.
 */
static void dropReply(state_t *st, const char *kind, const char *host,
                      int port, const char *line, long now) {

  int at = 0;
  char detail[256];

  for (; at < DROP_KINDS && st->dropKind[at] != NULL; ++at)
    if (strcmp(st->dropKind[at], kind) == 0)
      break;

  if (at == DROP_KINDS)
    at = DROP_KINDS - 1;

  if (st->dropKind[at] == NULL) {
    st->dropKind[at] = kind;
    st->dropAt[at] = LONG_MIN / 2;
  }

  if (now - st->dropAt[at] < DROP_LOG_MS) {
    st->dropped[at] += 1;
    return;
  }

  snprintf(detail, sizeof detail, "from %s:%d %.50s%s", host != NULL ? host : "?",
           port, line != NULL ? line : "",
           st->dropped[at] > 0 ? " (and more since)" : "");
  say(kind, detail);

  st->dropAt[at] = now;
  st->dropped[at] = 0;
}

/* -------------------------------------------------------------- targets */

static group_t *groupOf(state_t *st, const char *key) {

  group_t **found = st->groups.find(g, strcmp((*g)->key, key) == 0);

  return found != NULL ? *found : NULL;
}

static target_t *targetIn(group_t *g, const char *id) {

  target_t **found = g->targets.find(t, strcmp((*t)->id, id) == 0);

  return found != NULL ? *found : NULL;
}

static target_t *targetById(state_t *st, const char *id) {

  for (g in st->groups) {

    target_t *t = targetIn(*g, id);

    if (t != NULL)
      return t;
  }

  return NULL;
}

/** Takes a target out of a list, keeping the order of the rest. */
static void unlist(targets_t *list, target_t *t) {

  for (size_t i = 0; i < list->count; ++i)
    if (list->items[i] == t) {
      memmove(&list->items[i], &list->items[i + 1],
              (list->count - i - 1) * sizeof list->items[0]);
      list->count -= 1;
      return;
    }
}

static void freeTarget(target_t *t) {

  t->unsent.release();
  free(t->id);
  free(t->identifier);
  free(t->pin);
  free(t);
}

/**
 * A retired target goes once nothing needs it any more: no connection (its
 * reader lets go of it when its own calls are done), no dial under way, no
 * call waiting for one.
 */
static void settle(state_t *st, target_t *t) {

  if (t->group != NULL || t->conn != NULL || t->dialing || t->unsent.count > 0)
    return;

  unlist(&st->retired, t);
  freeTarget(t);
}

/** balance.js's removeTarget: out of the ring and the group, into retirement. */
static void retire(state_t *st, target_t *t) {

  group_t *g = t->group;

  if (g == NULL)
    return;

  unlist(&g->targets, t);

  if (t->node != NULL)
    g->ring.remove(t->node);

  t->node = NULL;
  t->group = NULL;

  event(st, "target-remove", t);

  /* nowhere to keep it: it is freed now if it can be, or leaks */
  st->retired.push(t);
  settle(st, t);
}

/* ------------------------------------------------------------ the pins */

/**
 * Puts a pin into the tree, holding a handler of ours or a group: its node
 * now goes to that, whatever it went to before - as index.js's add sets f.
 */
static void syncHandlers(state_t *st);

static void hold(state_t *st, mwm_handler_t handler, group_t *g,
                 const char *pin) {

  holder_t *h;

  /* our handlers were added before any client, as in a Node service */
  if (g != NULL)
    syncHandlers(st);

  h = g != NULL ? g->holder : NULL;

  if (st->pins == NULL && (st->pins = mwm_pinsNew()) == NULL)
    return;

  if (h == NULL) {

    h = (holder_t *)calloc(1, sizeof *h);

    if (h == NULL)
      return;

    if (!st->holders.push(h)) {
      free(h);
      return;
    }

    h->handler = handler;
    h->group = g;

    if (g != NULL)
      g->holder = h;
  }

  mwm_pinsAdd(st->pins, pin, h);
}

/**
 * Our handlers, in the order added, into the tree before the first call
 * and whenever there are more. A list that got shorter was cleared: the
 * tree is made again, our handlers first, then the groups oldest first.
 */
static void syncHandlers(state_t *st) {

  int count = mwm_handlerCount();

  if (count < st->handlersSeen) {

    mwm_pinsFree(st->pins);
    st->pins = NULL;

    for (h in st->holders)
      free(*h);

    st->holders.release();

    for (g in st->groups)
      (*g)->holder = NULL;

    for (int i = 0; i < count; ++i)
      hold(st, mwm_handlerAt(i), NULL, mwm_handlerPattern(i));

    for (g in st->groups)
      if ((*g)->targets.count > 0)
        hold(st, NULL, *g, (*g)->pin);

    st->handlersSeen = count;
    return;
  }

  for (int i = st->handlersSeen; i < count; ++i)
    hold(st, mwm_handlerAt(i), NULL, mwm_handlerPattern(i));

  st->handlersSeen = count;
}

/**
 * One member's pins as targets, or the reason it has none. mesh.js's
 * addClient: ourselves, null:true, a pin we serve (ignoreClient) are not
 * targets; transport.client makes a tcp client of everything that does not
 * say otherwise. A pin is config.pin[0], as makePinConfig takes it.
 */
static void member(state_t *st, int added, const char *identifier,
                   const uint8_t *bytes, size_t size) {

  json_t meta = mp_decode(bytes, size);
  defer meta.release();
  json_t config;
  json_t pinned;
  const char *pin;
  const char *type;
  const char *model;
  char *id;
  char *key;
  group_t *g;
  target_t *t;

  if (meta._refused != NULL)
    return;

  if (strcmp(meta.instance, st->options.instance) == 0)
    return;

  config = meta.config;
  pinned = config.pin;
  pin = pinned.count() > 0 && pinned._node != NULL && yyjson_mut_is_arr(pinned._node)
            ? pinned.at(0).text()
            : pinned.text();

  if (pin[0] == 0 || strcmp(pin, "null:true") == 0)
    return;

  for (int i = 0; i < st->ownPinCount; ++i)
    if (strcmp(st->ownPins[i], pin) == 0)
      return;

  if (config._node == NULL || !yyjson_mut_is_obj(config._node))
    return;

  id = targetId(config._node, pin, identifier);
  key = mwm_patternKey(pin);

  if (key == NULL) {
    free(id);
    return;
  }

  g = groupOf(st, key);

  if (!added) {

    t = g != NULL ? targetIn(g, id) : NULL;

    if (t != NULL) {
      retire(st, t);

      /**
       * microwizard fix/dead-pin: the pin whose last target left leaves the
       * tree, so a message falls through to the next best pin - a live
       * role:a,cmd:* rather than a dead role:a,cmd:x. A handler of ours on
       * the same pin holds the node again; a target visigoth only opened is
       * still in the group. The group stays, ring and all, for the next
       * member of the pin, whose target puts the pin back (hold).
       */
      if (g->targets.count == 0 && g->holder != NULL && st->pins != NULL)
        mwm_pinsRemove(st->pins, g->pin, g->holder);
    }

    free(id);
    free(key);
    return;
  }

  type = config.type;
  model = config.model;

  if (type[0] != 0 && strcmp(type, "tcp") != 0) {
    say("not a target, type is not tcp:", id);
    free(id);
    free(key);
    return;
  }

  /* observe is 1.6's: deliver to every target - not here (yet) */
  if (strcmp(model, "observe") == 0) {
    say("not a target, model observe is not supported:", id);
    free(id);
    free(key);
    return;
  }

  if (!dialable(st, config.host)) {
    say("not a target, outside the fence or not an address:", id);
    free(id);
    free(key);
    return;
  }

  /* a duplicate: mesh.js keeps the first */
  if (g != NULL && targetIn(g, id) != NULL) {
    free(id);
    free(key);
    return;
  }

  if (g == NULL) {

    g = (group_t *)calloc(1, sizeof *g);

    if (g == NULL) {
      free(id);
      free(key);
      return;
    }

    g->key = key;
    g->pin = textOf(pin);
    g->ring.init(st->options.balance.closingTimeout, 0);

    if (!st->groups.push(g)) {
      g->ring.release();
      free(g->key);
      free(g->pin);
      free(g);
      free(id);
      return;
    }
  } else
    free(key);

  t = (target_t *)calloc(1, sizeof *t);

  if (t == NULL) {
    free(id);
    return;
  }

  t->id = id;
  t->identifier = textOf(identifier);
  t->pin = textOf(pin);
  snprintf(t->host, sizeof t->host, "%s", config.host.text());
  t->port = config.port;

  /* {...settings, ...pinConfig}: what the member announces wins */
  t->timeout = config.timeout.isNothing() ? st->options.timeout
                                                  : config.timeout.number();
  t->connectTimeout = config.connectTimeout.isNothing()
                          ? st->options.connectTimeout
                          : config.connectTimeout.number();
  t->group = g;

  /* the end of the list: visigoth's ring is in insertion order too */
  if (!g->targets.push(t)) {
    freeTarget(t);
    return;
  }

  t->node = g->ring.add(t, 1, meta_nowMs());

  /* index.js adds the pin again for every client: its node holds this group */
  hold(st, NULL, g, pin);

  if (t->node == NULL)
    say("ring full, never chosen:", id);

  event(st, "target-add", t);
}

/* ---------------------------------------------------------- connections */

static char *requestLine(const char *id, const char *rest) {

  size_t room = strlen(id) + strlen(rest) + 16;
  char *line = (char *)malloc(room);

  if (line != NULL)
    snprintf(line, room, "{\"id\":\"%s\"%s", id, rest);

  return line;
}

/** The buffer, bigger - or NULL, and the buffer gone. */
static char *grown(char *in, size_t used, size_t room) {

  char *bigger = (char *)malloc(room);

  if (bigger != NULL && used > 0)
    memcpy(bigger, in, used);

  free(in);

  return bigger;
}

static char *maybe(const char *text) { return text != NULL ? strdup(text) : NULL; }

/** What a reader knows of itself without the router. */
typedef struct {
  char *targetId;
  char host[64];
  int port;
  long keepaliveMs;
  long keepaliveWaitMs;
  long tickMs;

  /* the table, by id: transfer.js's callmap for this connection */
  pending_t *[char *] waiting;
  long wrote;

  /* no entry is due before this (it may be earlier than any is) */
  long soonest;

  /* when to look at the router again */
  long lookAt;

  /* tcp.js's keepalive */
  long nextKeepalive;
  long checkAt;
  int traffic;
} side_t;

/** An answer to its caller, who owns it from here. */
META_FREES_ARG(1) static void deliver(replies_t *back, json_t got, char *target) {
  back->send((reply_t){RE_ANSWER, got, target});
}

/** Takes an entry out of the table once its call has its reply. */
static void forget(side_t *s, pending_t *p) {

  s->waiting.remove(p->id);
  free(p->id);
  free(p);
}

/** A call into the table; an id already there goes like no memory. */
static void enter(side_t *s, char *id, long until, replies_t *back) {

  pending_t *p = s->waiting.has(id) ? NULL : (pending_t *)calloc(1, sizeof *p);

  if (p == NULL) {
    back->send((reply_t){RE_TIMEOUT, {0}, maybe(s->targetId)});
    free(id);
    return;
  }

  p->id = id;
  p->until = until;
  p->back = back;

  if (until < s->soonest)
    s->soonest = until;

  if (!s->waiting.set(p->id, p)) {
    back->send((reply_t){RE_TIMEOUT, {0}, maybe(s->targetId)});
    free(p->id);
    free(p);
  }
}

/** Everything callers have said so far. */
static void drain(side_t *s, conn_t *c) {

  for (;;) {

    reg_t r;
    int got = 0;

    select {
    case one in c->regs:
      r = one;
      got = 1;
    default:
      got = 0;
    }

    if (!got)
      return;

    if (r.kind == RG_WROTE)
      s->wrote += 1;
    else
      enter(s, r.id, r.until, r.back);
  }
}

/**
 * Tells every waiting call `kind` - all of them, or those whose time is up.
 * Gathered first: the table moves entries as it loses one.
 */
static void expire(side_t *s, reply_kind_t kind, int all, long now) {

  pending_t *[] due;

  s->soonest = LONG_MAX;

  for (e in s->waiting)
    if (all || e->value->until <= now)
      due.push(e->value);
    else if (e->value->until < s->soonest)
      s->soonest = e->value->until;

  for (p in due) {
    (*p)->back->send((reply_t){kind, {0}, kind == RE_TIMEOUT ? maybe(s->targetId) : NULL});
    forget(s, *p);
  }

  due.release();
}

/**
 * handleResponse, with fix/unknown-reply: whatever comes back that cannot be
 * used is dropped and said once in a while; the connection stays.
 */
static void dropped(side_t *s, const char *kind, const char *text, long now) {

  state_t *st = take();

  if (st == NULL)
    return;

  dropReply(st, kind, s->host, s->port, text, now);
  give(st);
}

static void answer(side_t *s, conn_t *c, const char *text, size_t length,
                   long now) {

  json_t got = meta_toJSONn(text, length);
  json_t id;
  json_t sync;
  pending_t **p;

  /* a line that is not JSON never reaches the messager in tcp.js: no traffic */
  if (got._refused != NULL) {
    dropped(s, "invalid_json_res", text, now);
    got.release();
    return;
  }

  s->traffic = 1;

  if (got._node == NULL || !yyjson_mut_is_obj(got._node)) {
    dropped(s, "invalid_res", text, now);
    got.release();
    return;
  }

  /* the keepalive's answer: traffic, and nothing else */
  if (got.h._node != NULL && yyjson_mut_is_num(got.h._node) &&
      got.h.real() == 2) {
    got.release();
    return;
  }

  if (strcmp(got.k, "res") != 0 ||
      got.k._node == NULL || !yyjson_mut_is_str(got.k._node)) {
    dropped(s, "invalid_kind_res", text, now);
    got.release();
    return;
  }

  id = got.id;

  if (id._node != NULL && yyjson_mut_is_null(id._node)) {
    dropped(s, "no_message_id", text, now);
    got.release();
    return;
  }

  /**
   * sync false is accepted and nothing more: the call it answers goes on
   * waiting. An error in one is said, as unexcepted_async_error.
   */
  sync = got.sync;

  if (sync._node != NULL && !sync.truthy()) {
    if (got.error.truthy())
      dropped(s, "unexcepted_async_error", text, now);
    got.release();
    return;
  }

  /**
   * The id alone finds the call. Its entry is in the table, or still on the
   * way: a caller registers before it writes, so whatever it registered is
   * on the channel by now. An id that is not a string never matches one we
   * sent.
   */
  if (id._node != NULL && yyjson_mut_is_str(id._node)) {

    if ((p = s->waiting.get(id.text())) == NULL) {
      drain(s, c);
      p = s->waiting.get(id.text());
    }

    if (p != NULL) {
      pending_t *waiting = *p;

      /* the target goes with an error only: an overload or a timeout opens it */
      char *target = got.error.truthy() ? maybe(s->targetId) : NULL;

      deliver(waiting->back, got, target);
      forget(s, waiting);
      return;
    }
  }

  dropped(s, "unknown_message_id", text, now);
  got.release();
}

/**
 * tcp.js's keepalive: every two minutes {"h":1}; five seconds later, if no
 * line at all came in since the last look, the connection is dropped and
 * only made again for the next call. The flag is reset at the look, not at
 * the {"h":1}, as keepalive.called is.
 */
static void keepalive(side_t *s, conn_t *c, long now) {

  if (s->checkAt != 0 && now >= s->checkAt) {

    s->checkAt = 0;

    if (s->traffic)
      s->traffic = 0;
    else {
      c->line.shutdown();
      return;
    }
  }

  if (now >= s->nextKeepalive) {
    if (!c->line.writeLine("{\"h\":1}")) {
      c->line.shutdown();
      return;
    }
    s->checkAt = now + s->keepaliveWaitMs;
    s->nextKeepalive = now + s->keepaliveMs;
  }
}

/**
 * The reader's look at the router, once a tick and at the end: what callers
 * said, and whether its connection is still wanted - not when the client
 * stops (every call is told), not when its target is retired and nobody
 * waits. Answers whether it is done: let go, nothing waiting, nobody
 * writing.
 */
static int look(side_t *s, conn_t *c, int ended) {

  state_t *st = take();
  int done;

  if (st == NULL) {
    /* cannot happen: the router outlives every reader */
    drain(s, c);
    expire(s, RE_CLOSED, 1, 0);
    return s->wrote >= c->users;
  }

  drain(s, c);

  if (st->stopping) {
    expire(s, RE_CLOSED, 1, 0);
    ended = 1;
  }

  if (c->target != NULL &&
      (ended || (c->target->group == NULL && s->waiting.count == 0 &&
                 s->wrote == c->users))) {

    target_t *t = c->target;

    /* no new callers from here: a REG sent so far is drained below */
    t->conn = NULL;
    c->target = NULL;
    c->line.shutdown();
    drain(s, c);

    if (t->group == NULL)
      settle(st, t);
  }

  done = c->target == NULL && s->waiting.count == 0 && s->wrote == c->users;

  give(st);

  return done;
}

/**
 * One connection's life: dial, take over the calls that waited for it, read
 * answers until the end, then wait until nothing is waiting and nobody is
 * writing, close. Reads with its own deadline - the tick - because a
 * connection's timeout bounds its writers too: recv on the descriptor
 * while there is something, a wait until readable or the tick when not.
 */
static void reader(target_t *t, char *host, int port, long connectMs,
                   long ioMs, mwm_client_options_t options, char *targetId) {

  conn_t c;
  regs_t regs;
  side_t s;
  char *in = NULL;
  size_t room = 0;
  size_t start = 0;
  size_t end = 0;
  size_t looked = 0;
  unsent_t *[] taken;
  state_t *st;

  memset(&s, 0, sizeof s);
  s.soonest = LONG_MAX;
  s.targetId = targetId;
  snprintf(s.host, sizeof s.host, "%s", host);
  s.port = port;
  s.keepaliveMs = options.keepaliveMs;
  s.keepaliveWaitMs = options.keepaliveWaitMs;
  s.tickMs = options.tickMs > 0 ? options.tickMs : 100;

  c.regs = &regs;
  c.line = meta_tcpDial(host, port, connectMs);
  c.target = NULL;
  c.users = 0;
  free(host);

  st = take();

  if (st == NULL) {
    c.line.close();
    free(targetId);
    return;
  }

  t->dialing = 0;

  if (c.line.failed()) {
    say("cannot connect:", c.line.error());
    c.line.close();
    settle(st, t);
    give(st);
    free(targetId);
    return;
  }

  /* nobody wants it any more */
  if (st->stopping || (t->group == NULL && t->unsent.count == 0)) {
    c.line.close();
    settle(st, t);
    give(st);
    free(targetId);
    return;
  }

  /* bounds every write onto it, the callers' as well as the keepalive */
  c.line.timeout(ioMs);

  t->conn = &c;
  c.target = t;

  for (u in t->unsent)
    taken.push(*u);

  t->unsent.count = 0;
  give(st);

  {
    long now = meta_nowMs();

    s.nextKeepalive = now + s.keepaliveMs;

    /* tcp.js: timeout runs from when the line is out */
    for (u in taken) {

      unsent_t *w = *u;

      enter(&s, w->id, now + w->timeout, w->back);

      if (!c.line.writeLine(w->line))
        c.line.shutdown();

      free(w->line);
      free(w);
    }

    taken.release();
  }

  s.lookAt = meta_nowMs() + s.tickMs;

  for (;;) {

    char *nl;
    ssize_t got;
    long now;
    long wake = s.soonest;

    if (s.nextKeepalive < wake)
      wake = s.nextKeepalive;

    if (s.checkAt != 0 && s.checkAt < wake)
      wake = s.checkAt;

    if (s.lookAt < wake)
      wake = s.lookAt;

    /* every whole line there is */
    while (end > start + looked &&
           (nl = (char *)memchr(in + start + looked, '\n',
                                end - start - looked)) != NULL) {

      size_t length = (size_t)(nl - (in + start));

      if (length > 0 && in[start + length - 1] == '\r')
        length -= 1;

      in[start + length] = 0;

      if (length > 0)
        answer(&s, &c, in + start, length, meta_nowMs());

      start = (size_t)(nl - in) + 1;
      looked = 0;
    }

    looked = end - start;

    /* a line longer than that is not waited for: where the next begins is not known */
    if (looked > LONGEST)
      break;

    if (start == end)
      start = end = 0;
    else if (start > 0 && end == room) {
      memmove(in, in + start, end - start);
      end -= start;
      start = 0;
    }

    if (end == room) {

      room = room != 0 ? room * 2 : READ_CHUNK;

      if ((in = grown(in, end, room)) == NULL)
        break;
    }

    got = recv(c.line.fd(), in + end, room - end, MSG_DONTWAIT);

    if (got > 0) {
      end += (size_t)got;
      if (meta_nowMs() < wake)
        continue;
    } else if (got == 0)
      break;
    else if (errno == EINTR)
      continue;
    else if (errno != EAGAIN && errno != EWOULDBLOCK)
      break;
    else if (meta_nowMs() < wake && (@readable(c.line.fd(), wake)) == 0)
      continue;

    now = meta_nowMs();

    if (now < wake)
      continue;

    /* what woke it: its calls' clocks, the keepalive, the router */
    if (now >= s.soonest)
      expire(&s, RE_TIMEOUT, 0, now);

    keepalive(&s, &c, now);

    if (now >= s.lookAt) {
      s.lookAt = now + s.tickMs;
      if (look(&s, &c, 0))
        break;
    }
  }

  /* the end: no new callers, and the ones there are get their answers or their time */
  free(in);

  while (!look(&s, &c, 1)) {
    expire(&s, RE_TIMEOUT, 0, meta_nowMs());
    meta_sleepMs(s.tickMs);
  }

  c.line.close();
  s.waiting.release();
  free(targetId);
}

static void dial(state_t *st, target_t *t, long now) {

  long connectMs = t->connectTimeout > 0 ? t->connectTimeout : 355;
  long ioMs = t->timeout > 0 ? t->timeout : 5555;

  t->dialing = 1;
  t->lastDial = now;

  go clientScope reader(t, strdup(t->host), t->port, connectMs, ioMs,
                        st->options, strdup(t->id));
}

/* ---------------------------------------------------------------- calls */

/**
 * #matchPin over ours and the mesh's in one tree (pattern.h): a handler of
 * ours for role:a,cmd:ping beats a group for role:a,cmd:*, a group for
 * role:x,cmd:* beats a handler of ours for role:x.
 */
static holder_t *holderFor(state_t *st, mwm_parts_t *parts) {

  syncHandlers(st);

  return (holder_t *)mwm_pinsMatch(st->pins, parts);
}

/**
 * One try, in the caller's task: the match and the choice with the router
 * in hand, then the request - onto the target's connection by the caller
 * itself, or into the target to wait for one. RE_WAIT when the reply is to
 * come on back; anything else is the reply, there and then. Takes id.
 */
static reply_t attempt(mwm_parts_t *parts, char *id, const char *rest,
                       long maxWait, replies_t *back) {

  state_t *st = take();
  holder_t *h;
  group_t *g;
  vg_node_t *node;
  target_t *t;
  conn_t *c;
  long timeout;
  long connectMs;
  long now;
  char *line;

  if (st == NULL) {
    free(id);
    return (reply_t){RE_CLOSED, {0}, NULL};
  }

  if (st->stopping) {
    give(st);
    free(id);
    return (reply_t){RE_CLOSED, {0}, NULL};
  }

  h = holderFor(st, parts);

  if (h != NULL && h->handler != NULL) {
    mwm_handler_t handler = h->handler;

    give(st);
    free(id);
    return (reply_t){RE_LOCAL, {0}, NULL, handler};
  }

  now = meta_nowMs();
  g = h != NULL ? h->group : NULL;

  if (g == NULL || (node = g->ring.choose(now)) == NULL) {
    give(st);
    free(id);
    return (reply_t){g == NULL ? RE_NO_PIN : RE_NO_TARGET, {0}, NULL};
  }

  t = (target_t *)node->target;

  /* chosen, as balance.js chooses before it looks at what is left */
  if (maxWait == 0) {
    give(st);
    free(id);
    return (reply_t){RE_SPENT, {0}, NULL};
  }

  timeout = t->timeout;
  connectMs = t->connectTimeout;

  if (maxWait > 0) {
    timeout = timeout < maxWait ? timeout : maxWait;
    connectMs = connectMs < maxWait ? connectMs : maxWait;
  }

  line = requestLine(id, rest);

  if (line == NULL) {
    reply_t r = {RE_TIMEOUT, {0}, strdup(t->id)};

    give(st);
    free(id);
    return r;
  }

  if ((c = t->conn) != NULL) {

    /* the reader keeps the connection until this caller says WROTE */
    c->users += 1;
    give(st);

    c->regs->send((reg_t){RG_CALL, id, now + timeout, back});

    if (!c->line.writeLine(line))
      c->line.shutdown();

    c->regs->send((reg_t){RG_WROTE, NULL, 0, NULL});
    free(line);

    return (reply_t){RE_WAIT, {0}, NULL};
  }

  /**
   * tcp.js: connectTimeout runs from the call until the connection is
   * there, then timeout until the answer. A connection that cannot be had
   * is a timeout after connectTimeout, not an error at once - the call
   * waits for reconnect-core's next try as it would in Node.
   */
  {
    unsent_t *u = (unsent_t *)calloc(1, sizeof *u);

    if (u == NULL) {
      reply_t r = {RE_TIMEOUT, {0}, strdup(t->id)};

      give(st);
      free(id);
      free(line);
      return r;
    }

    u->id = id;
    u->line = line;
    u->timeout = timeout;
    u->until = now + connectMs;
    u->back = back;

    if (!t->unsent.push(u)) {
      reply_t r = {RE_TIMEOUT, {0}, strdup(t->id)};

      give(st);
      free(u->id);
      free(u->line);
      free(u);
      return r;
    }

    if (!t->dialing)
      dial(st, t, now);
  }

  give(st);

  return (reply_t){RE_WAIT, {0}, NULL};
}

/** visigoth's failure handler for the target a try went to. */
static void failed(char *id) {

  state_t *st = take();
  target_t *t;

  if (st != NULL) {
    t = targetById(st, id);

    if (t != NULL && t->node != NULL)
      t->node->failed(meta_nowMs());

    give(st);
  }

  free(id);
}

/* ---------------------------------------------------------------- clock */

/**
 * The calls of a target that wait for a connection: those whose time is up
 * are told (all of them, when the client stops), and a target with calls
 * left and no connection being made dials again.
 */
static void waitingFor(state_t *st, target_t *t, reply_kind_t kind, int all,
                       long now) {

  size_t kept = 0;

  for (size_t i = 0; i < t->unsent.count; ++i) {

    unsent_t *u = t->unsent.items[i];

    if (!all && u->until > now) {
      t->unsent.items[kept++] = u;
      continue;
    }

    u->back->send((reply_t){kind, {0}, kind == RE_TIMEOUT ? strdup(t->id) : NULL});
    free(u->id);
    free(u->line);
    free(u);
  }

  t->unsent.count = kept;

  if (!all && kept > 0 && !t->dialing && t->conn == NULL &&
      now - t->lastDial >= REDIAL_MS)
    dial(st, t, now);
}

static void tick(state_t *st, long now) {

  for (g in st->groups)
    for (t in (*g)->targets)
      if ((*t)->unsent.count > 0)
        waitingFor(st, *t, RE_TIMEOUT, 0, now);

  for (size_t i = 0; i < st->retired.count;) {

    target_t *t = st->retired.items[i];

    if (t->unsent.count > 0)
      waitingFor(st, t, RE_TIMEOUT, 0, now);

    /* settle may take it out of the list */
    settle(st, t);

    if (i < st->retired.count && st->retired.items[i] == t)
      i += 1;
  }
}

/** Stops: every call that waits for a connection is told, every target retired. */
static void stop(state_t *st) {

  st->stopping = 1;

  for (gp in st->groups) {

    group_t *g = *gp;

    for (t in g->targets) {
      waitingFor(st, *t, RE_CLOSED, 1, 0);

      /* its reader wakes to the end, and tells its calls */
      if ((*t)->conn != NULL)
        (*t)->conn->line.shutdown();
    }

    while (g->targets.count > 0)
      retire(st, g->targets.items[0]);

    g->targets.release();
    g->ring.release();
    free(g->key);
    free(g->pin);
    free(g);
  }

  st->groups.release();

  for (t in st->retired) {
    waitingFor(st, *t, RE_CLOSED, 1, 0);
    if ((*t)->conn != NULL)
      (*t)->conn->line.shutdown();
  }
}

static void ticker(long ms) {

  for (;;) {

    state_t *st;

    meta_sleepMs(ms);

    if ((st = take()) == NULL)
      return;

    if (st->stopping) {
      give(st);
      return;
    }

    tick(st, meta_nowMs());
    give(st);
  }
}

/* ------------------------------------------------------------ the API */

void mwm_clientDefaults(mwm_client_options_t *into) {

  memset(into, 0, sizeof *into);
  bal_defaults(&into->balance);
  into->keepaliveMs = 2L * 60 * 1000;
  into->keepaliveWaitMs = 5000;
  into->timeout = 5555;
  into->connectTimeout = 355;
  into->tickMs = 100;

  /* a patient call's budget follows timeout, as Micro hands its own over */
  into->balance.budget = 0;
}

int mwm_clientStart(const mwm_client_options_t *options) {

  state_t *st;

  if (started)
    return 0;

  st = (state_t *)calloc(1, sizeof *st);

  if (st == NULL)
    return 0;

  st->ownPins = (char **)calloc((size_t)options->ownPinCount + 1, sizeof *st->ownPins);

  if (st->ownPins == NULL) {
    free(st);
    return 0;
  }

  for (int i = 0; i < options->ownPinCount; ++i)
    st->ownPins[i] = strdup(options->ownPins[i]);

  st->ownPinCount = options->ownPinCount;
  st->options = *options;
  st->options.ownPins = NULL;

  balanceOptions = options->balance;

  if (balanceOptions.budget <= 0)
    balanceOptions.budget = options->timeout > 0 ? options->timeout : 5555;

  started = 1;

  router.send(st);

  go clientScope ticker(options->tickMs > 0 ? options->tickMs : 100);

  return 1;
}

void mwm_clientStop(void) {

  state_t *st;

  if (!started || (st = take()) == NULL)
    return;

  stop(st);
  give(st);

  /* the readers tell their calls and close, the ticker sees it is over */
  join clientScope;

  if ((st = take()) == NULL)
    return;

  /* from here a call or a member finds no router */
  router.close();

  for (t in st->retired)
    freeTarget(*t);

  st->retired.release();

  mwm_pinsFree(st->pins);

  for (h in st->holders)
    free(*h);

  st->holders.release();

  for (int i = 0; i < st->ownPinCount; ++i)
    free(st->ownPins[i]);

  free(st->ownPins);
  free(st);
}

void mwm_clientMember(int added, const char *identifier, const uint8_t *meta,
                      size_t metaLen) {

  state_t *st;

  /* before the start or after the stop: nobody to tell */
  if (!started || (st = take()) == NULL)
    return;

  if (!st->stopping)
    member(st, added, identifier, meta, metaLen);

  give(st);
}

static json_t rejection(const char *err, const char *errDocument,
                        const char *msg) {

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *out = yyjson_mut_obj(doc);
  yyjson_doc *read;

  yyjson_mut_doc_set_root(doc, out);

  if (errDocument != NULL &&
      (read = yyjson_read(errDocument, strlen(errDocument), 0)) != NULL) {
    yyjson_mut_obj_add(out, yyjson_mut_str(doc, "err"),
                       yyjson_val_mut_copy(doc, yyjson_doc_get_root(read)));
    yyjson_doc_free(read);
  } else
    yyjson_mut_obj_add(out, yyjson_mut_str(doc, "err"),
                       yyjson_mut_strcpy(doc, err));

  if (msg != NULL && (read = yyjson_read(msg, strlen(msg), 0)) != NULL) {
    yyjson_mut_obj_add(out, yyjson_mut_str(doc, "msg"),
                       yyjson_val_mut_copy(doc, yyjson_doc_get_root(read)));
    yyjson_doc_free(read);
  }

  return meta_jsonFromMut(doc);
}

/** A copy of one value of a parsed answer as a document of its own. */
static json_t copyOf(yyjson_mut_val *value) {

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

  if (value != NULL && doc != NULL)
    yyjson_mut_doc_set_root(doc, yyjson_mut_val_mut_copy(doc, value));

  return meta_jsonFromMut(doc);
}

/** The error the service sent, as balance.js tells overload and timeout apart. */
static int retryable(json_t error) {

  return strcmp(error.details.message,
                "retry_later_err_overload") == 0 ||
         strcmp(error.message, "timeout") == 0;
}

/** A rejection as actAsync settles with it. */
static mwm_settled_t rejected(const char *err, const char *errDocument,
                              const char *msg) {

  json_t value = rejection(err, errDocument, msg);

  return (mwm_settled_t){1, value};
}

/** A handler of ours answers here, as a local act does in Node. */
static mwm_settled_t local(mwm_handler_t handler, json_t data) {

  mwm_answer_t answer = handler(data);

  return (mwm_settled_t){answer.thrown, answer.value};
}


/**
 * balance.js consume, in the caller's task: choose and send (attempt),
 * wait for the one reply when there is one to wait for, and ask balance.c
 * what next. A retry is a new request with an id of its own. When a handler
 * of ours wins the match, it answers here with data, as a local act.
 * shown is what no-target is logged with.
 */
static mwm_settled_t consume(const char *shown, mwm_parts_t *parts, char *rest,
                             const char *msg, json_t data) {

  replies_t back;
  bal_consume_t step;
  char id[16];
  mwm_settled_t out = {0, {NULL, NULL, NULL, "", NULL, 0, NULL}};

  bal_begin(&step, &balanceOptions, meta_nowMs());
  makeId(id);

  for (;;) {

    reply_t r;
    long maxWait;
    json_t got;
    json_t error;

    if (step.wait > 0)
      meta_sleepMs(step.wait);

    /* 0 for a patient call whose budget is spent: chosen, and over */
    maxWait = bal_tryTimeout(&step, LONG_MAX, meta_nowMs());

    if (bal_isRetry(&step))
      makeId(id);

    r = attempt(parts, strdup(id), rest, maxWait == LONG_MAX ? -1 : maxWait,
                &back);

    if (r.kind == RE_WAIT && !back.receive(&r)) {
      out = rejected("client-stopped", NULL, msg);
      break;
    }

    if (r.kind == RE_NO_PIN) {
      /* index.js: console.log('no-target', pat), resolved undefined */
      say("no-target", shown);
      json_t none = copyOf(NULL);
      out = (mwm_settled_t){0, none};
      break;
    }

    if (r.kind == RE_LOCAL) {
      out = local(r.handler, data);
      break;
    }

    if (r.kind == RE_CLOSED) {
      free(r.target);
      out = rejected("client-stopped", NULL, msg);
      break;
    }

    if (r.kind == RE_SPENT) {
      out = rejected("all-targets-overloaded", NULL, msg);
      break;
    }

    if (r.kind == RE_NO_TARGET) {
      if (bal_noTarget(&step, meta_nowMs()) == BAL_DONE) {
        out = rejected(bal_outcomeText(step.outcome), NULL, msg);
        break;
      }
      continue;
    }

    if (r.kind == RE_TIMEOUT) {
      if (r.target != NULL)
        failed(r.target);
      if (bal_tried(&step, BAL_RETRYABLE, meta_nowMs()) == BAL_DONE) {
        out = rejected(bal_outcomeText(step.outcome), NULL, msg);
        break;
      }
      continue;
    }

    /* an answer */
    got = r.got;
    error = got.error;

    /* `!data.error`: an error that is null, false, 0 or "" is no error */
    if (error.truthy()) {

      if (retryable(error)) {
        if (r.target != NULL) {
          failed(r.target);
          r.target = NULL;
        }
        got.release();
        if (bal_tried(&step, BAL_RETRYABLE, meta_nowMs()) == BAL_DONE) {
          out = rejected(bal_outcomeText(step.outcome), NULL, msg);
          break;
        }
        continue;
      }

      {
        char *text = yyjson_mut_val_write(error._node, 0, NULL);

        out = rejected(NULL, text, msg);
        free(text);
      }
      free(r.target);
      got.release();
      break;
    }

    {
      json_t res = copyOf(got.res._node);
      out = (mwm_settled_t){0, res};
    }
    free(r.target);
    got.release();
    break;
  }

  free(rest);

  return out;
}

/**
 * Object.assign({}, xConv, y): the pattern's pairs as strings, then the
 * args over them - a key already there keeps its place and takes the new
 * value, a new key goes to the end.
 */
static json_t merged(const char *pattern, json_t args) {

  json_t data = pattern != NULL ? mwm_patternObject(pattern)
                                : meta_jsonFromMut(yyjson_mut_doc_new(NULL));
  yyjson_mut_doc *doc = data._doc;
  yyjson_mut_val *root = data._node;

  if (doc == NULL)
    return data;

  if (root == NULL) {
    root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    data._node = root;
  }

  if (args._node != NULL && yyjson_mut_is_obj(args._node)) {

    yyjson_mut_obj_iter it;
    yyjson_mut_val *key;

    yyjson_mut_obj_iter_init(args._node, &it);

    while ((key = yyjson_mut_obj_iter_next(&it)) != NULL) {

      yyjson_mut_val *value =
          yyjson_mut_val_mut_copy(doc, yyjson_mut_obj_iter_get_val(key));
      yyjson_mut_val *name = yyjson_mut_strcpy(doc, yyjson_mut_get_str(key));

      if (!yyjson_mut_obj_replace(root, name, value))
        yyjson_mut_obj_add(root, name, value);
    }
  }

  return data;
}

/**
 * Without the client's tasks there is no mesh: our handlers alone, as
 * service.c matches them.
 */
static mwm_settled_t unstarted(const char *shown, mwm_parts_t *parts,
                               json_t data) {

  mwm_handler_t handler = mwm_findHandler(parts);

  if (handler != NULL)
    return local(handler, data);

  say("no-target", shown);

  {
    json_t none = copyOf(NULL);
    return (mwm_settled_t){0, none};
  }
}

/**
 * act(pattern, args): the message is the pattern's pairs as convertData
 * finds them and the primitive fields of args - both, even where args has
 * another value for a key of the pattern; what is sent is the pattern as an
 * object with args laid over it.
 */
mwm_settled_t mwm_act(const char *pattern, json_t args) {

  json_t data = merged(pattern, args);
  defer data.release();
  mwm_parts_t parts;
  char *text;
  char *rest;
  size_t room;
  mwm_settled_t out;

  mwm_partsInit(&parts);
  mwm_partsForAct(&parts, pattern, args);

  text = yyjson_mut_val_write(data._node, 0, NULL);

  if (text == NULL) {
    mwm_partsFree(&parts);
    return rejected("no-memory", NULL, NULL);
  }

  if (!started) {
    out = unstarted(pattern != NULL ? pattern : text, &parts, data);
    mwm_partsFree(&parts);
    free(text);
    return out;
  }

  room = strlen(text) + 64;
  rest = (char *)malloc(room);

  if (rest == NULL) {
    mwm_partsFree(&parts);
    free(text);
    return rejected("no-memory", NULL, NULL);
  }

  /* prepareRequest's order: id, k, sync, args */
  snprintf(rest, room, ",\"k\":\"a\",\"sync\":true,\"args\":%s}", text);

  out = consume(pattern != NULL ? pattern : text, &parts, rest, text, data);

  mwm_partsFree(&parts);
  free(text);
  return out;
}

/** actE(pattern, data): the message is the pattern's pairs alone. */
mwm_settled_t mwm_actE(const char *pattern, json_t data) {

  mwm_parts_t parts;
  char *d = NULL;
  char *p;
  char *rest;
  size_t room;
  mwm_settled_t out;

  mwm_partsInit(&parts);
  mwm_partsFromPattern(&parts, pattern);

  if (!started) {
    out = unstarted(pattern != NULL ? pattern : "", &parts, data);
    mwm_partsFree(&parts);
    return out;
  }

  if (data._node != NULL)
    d = yyjson_mut_val_write(data._node, 0, NULL);

  {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

    p = yyjson_mut_val_write(yyjson_mut_strcpy(doc, pattern != NULL ? pattern : ""),
                             0, NULL);
    yyjson_mut_doc_free(doc);
  }

  room = (d != NULL ? strlen(d) : 0) + strlen(p) + 64;
  rest = (char *)malloc(room);

  if (rest == NULL) {
    mwm_partsFree(&parts);
    free(d);
    free(p);
    return rejected("no-memory", NULL, NULL);
  }

  /* prepareRequest: id, k, sync, p, d - and d left out when undefined */
  if (d != NULL)
    snprintf(rest, room, ",\"k\":\"aE\",\"sync\":true,\"p\":%s,\"d\":%s}", p, d);
  else
    snprintf(rest, room, ",\"k\":\"aE\",\"sync\":true,\"p\":%s}", p);

  free(p);

  out = consume(pattern != NULL ? pattern : "", &parts, rest, d, data);

  mwm_partsFree(&parts);
  free(d);
  return out;
}
