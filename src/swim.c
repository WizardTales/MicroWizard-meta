/**
 * swim-ng's Membership, FailureDetector, Disseminator and Net in one place,
 * with the timers they set as entries in one list instead of setTimeouts.
 *
 * The order of things follows the JS closely enough that a reader with
 * lib/membership.js open beside this can follow along; where the JS does
 * something odd it says so here, and docs/protocol.md says it again.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msgpack.h"
#include "swim.h"

/* --------------------------------------------------------------- the state */

typedef enum {
  TIMER_TICK,          /* the failure detector's setInterval */
  TIMER_PING,          /* no ack to our ping: ping-req (seq) */
  TIMER_SEQ,           /* a ping-req or a forwarded ping gives up (seq) */
  TIMER_PING_REQ,      /* no relayed ack either: suspect (group) */
  TIMER_SUSPECT,       /* suspect long enough: faulty (host) */
  TIMER_JOIN_CHECK,    /* has a base answered yet */
  TIMER_JOIN_TIMEOUT   /* none did */
} timer_kind_t;

typedef struct {
  long when;
  timer_kind_t kind;
  uint32_t seq;
  uint32_t group;
  char host[SWIM_HOST_MAX];

  /** The member as it was when the timer was set - what the JS closes over. */
  int64_t incarnation;
} swim_timer_t;

typedef enum {
  /** An ack for a ping-req we sent through a relay: the target lives. */
  CALLBACK_RELAYED,

  /** An ack for a ping we sent for somebody's ping-req: tell them. */
  CALLBACK_FORWARD
} callback_kind_t;

typedef struct {
  callback_kind_t kind;
  uint32_t group;
  char host[SWIM_HOST_MAX];
  int64_t seq;
} callback_t;

typedef struct {
  char host[SWIM_HOST_MAX];
  uint8_t *bytes;
  size_t size;
  int attempts;

  /** When it went into its attempts bucket - the order Object.keys gives. */
  uint64_t order;
} update_t;

typedef struct {
  char host[SWIM_HOST_MAX];
} hostName_t;

struct swim_t {
  swim_options_t options;
  swim_hooks_t hooks;

  swim_member_t local;

  /** hostToMember: alive and suspect. The key is the member's own host. */
  swim_member_t *[char *] members;

  /** hostToFaulty. Never emptied, as in swim-ng. */
  swim_member_t *[char *] faulty;

  /** hostToIterable: who is still to be pinged this round. */
  hostName_t[] iterable;

  swim_timer_t[] timers;
  callback_t[size_t] callbacks;
  update_t[] updates;

  uint32_t seq;
  uint32_t groups;
  uint64_t order;
  int disseminationLimit;

  int started;
  hostName_t[] joining;
  int joinPending;

  mp_buf_t scratch;
};

/* ------------------------------------------------------------- the helpers */

void swim_defaults(swim_options_t *into) {

  into->interval = 333;
  into->pingTimeout = 444;
  into->pingReqTimeout = 555;
  into->pingReqGroupSize = 7;
  into->suspectTimeout = 999;
  into->joinTimeout = 999;
  into->joinCheckInterval = 50;
  into->disseminationFactor = 22;
  into->maxDgramSize = 2048;
}

static void copyHost(char *into, const char *from, size_t size) {

  if (size >= SWIM_HOST_MAX)
    size = SWIM_HOST_MAX - 1;

  memcpy(into, from, size);
  into[size] = 0;
}

/** Copies first and frees after, so `meta` may be the member's own. */
static void setMeta(swim_member_t *m, const uint8_t *meta, size_t size) {

  uint8_t *copy = NULL;

  if (meta == m->meta && size == m->metaLen)
    return;

  if (meta != NULL && size != 0) {

    copy = (uint8_t *)malloc(size);

    if (copy != NULL)
      memcpy(copy, meta, size);
  }

  free(m->meta);
  m->meta = copy;
  m->metaLen = copy != NULL ? size : 0;
}

/**
 * Member.data() as msgpackr packs it: {meta, host, state, incarnation}, a
 * map16 in that order. A member without meta has `meta: undefined` in JS,
 * which msgpackr writes as fixext 1 type 0 - so that is what goes out here.
 */
static void writeData(mp_buf_t *out, const swim_member_t *m) {

  out->map(4);
  out->cstr("meta");

  if (m->meta != NULL)
    out->raw(m->meta, m->metaLen);
  else {
    static const uint8_t undefined[3] = {0xd4, 0x00, 0x00};
    out->raw(undefined, sizeof undefined);
  }

  out->cstr("host");
  out->cstr(m->host);
  out->cstr("state");
  out->integer(m->state);
  out->cstr("incarnation");
  out->integer(m->incarnation);
}

static void emit(swim_t *self, swim_event_t event, const swim_member_t *m);

/* ---------------------------------------------------------------- timers */

static void timerAdd(swim_t *self, long when, timer_kind_t kind, uint32_t seq,
                     uint32_t group, const char *host, int64_t incarnation) {

  swim_timer_t t;

  memset(&t, 0, sizeof t);
  t.when = when;
  t.kind = kind;
  t.seq = seq;
  t.group = group;
  t.incarnation = incarnation;

  if (host != NULL)
    copyHost(t.host, host, strlen(host));

  self->timers.push(t);
}

static void timerDrop(swim_t *self, size_t at) {

  self->timers[at] = self->timers[self->timers.count - 1];
  self->timers.count -= 1;
}

/** clearTimeout for a seq: every kind that carries one. */
static void timersClearSeq(swim_t *self, uint32_t seq) {

  for (size_t at = 0; at < self->timers.count;) {

    timer_kind_t kind = self->timers[at].kind;

    if ((kind == TIMER_PING || kind == TIMER_SEQ) && self->timers[at].seq == seq)
      timerDrop(self, at);
    else
      ++at;
  }
}

static void timersClear(swim_t *self, timer_kind_t kind, uint32_t group,
                        const char *host) {

  for (size_t at = 0; at < self->timers.count;) {

    swim_timer_t *t = &self->timers[at];
    int hit = t->kind == kind &&
              (kind == TIMER_PING_REQ ? t->group == group
               : host != NULL         ? strcmp(t->host, host) == 0
                                      : 1);

    if (hit)
      timerDrop(self, at);
    else
      ++at;
  }
}

static int hasTimer(swim_t *self, timer_kind_t kind, const char *host) {

  for (size_t at = 0; at < self->timers.count; ++at)
    if (self->timers[at].kind == kind && strcmp(self->timers[at].host, host) == 0)
      return 1;

  return 0;
}

long swim_nextDeadline(const swim_t *self) {

  long next = -1;

  for (size_t at = 0; at < self->timers.count; ++at)
    if (next < 0 || self->timers[at].when < next)
      next = self->timers[at].when;

  return next;
}

/* ----------------------------------------------------------- dissemination */

/** ceil(factor * log10(size + 1)), size counting us. */
static void updateDisseminationLimit(swim_t *self) {

  size_t size = self->members.count + 1;

  self->disseminationLimit =
      (int)ceil(self->options.disseminationFactor * log((double)size + 1) /
                log(10));

  for (size_t at = 0; at < self->updates.count;) {

    if (self->updates[at].attempts >= self->disseminationLimit) {
      free(self->updates[at].bytes);
      self->updates[at] = self->updates[self->updates.count - 1];
      self->updates.count -= 1;
    } else
      ++at;
  }
}

static void removeUpdate(swim_t *self, const char *host) {

  for (size_t at = 0; at < self->updates.count; ++at) {

    if (strcmp(self->updates[at].host, host) == 0) {
      free(self->updates[at].bytes);
      self->updates[at] = self->updates[self->updates.count - 1];
      self->updates.count -= 1;
      return;
    }
  }
}

/** Disseminator.onUpdate: the newest word about a host replaces the last. */
static void addUpdate(swim_t *self, const swim_member_t *m) {

  update_t u;

  removeUpdate(self, m->host);

  if (self->disseminationLimit <= 0)
    return;

  self->scratch.reset();
  self->scratch.byte(SWIM_UPDATE);
  writeData(&self->scratch, m);

  if (self->scratch.failed)
    return;

  memset(&u, 0, sizeof u);
  copyHost(u.host, m->host, strlen(m->host));
  u.bytes = (uint8_t *)malloc(self->scratch.len);

  if (u.bytes == NULL)
    return;

  memcpy(u.bytes, self->scratch.data, self->scratch.len);
  u.size = self->scratch.len;
  u.attempts = 0;
  u.order = self->order++;

  self->updates.push(u);
}

static int byAttemptsThenOrder(const void *a, const void *b) {

  const update_t *x = *(const update_t *const *)a;
  const update_t *y = *(const update_t *const *)b;

  if (x->attempts != y->attempts)
    return x->attempts < y->attempts ? -1 : 1;

  return x->order < y->order ? -1 : x->order > y->order ? 1 : 0;
}

/**
 * getUpdatesUpTo: the least-told updates first, as many as fit. Each one
 * taken is told once more; one told disseminationLimit times is dropped.
 * Appends them to `parts` as indexes into self->updates.
 */
static size_t takeUpdates(swim_t *self, long bytesAvailable,
                          const update_t **parts, size_t room) {

  const update_t *[] order;
  size_t taken = 0;
  size_t count = self->updates.count;

  if (count == 0)
    return 0;

  for (u in self->updates)
    if (!order.push(u)) {
      order.release();
      return 0;
    }

  qsort(order.items, count, sizeof order[0], byAttemptsThenOrder);

  for (size_t at = 0; at < count && taken < room; ++at) {

    if (bytesAvailable <= 1)
      break;

    if (order[at]->attempts >= self->disseminationLimit)
      continue;

    if ((long)order[at]->size + 2 <= bytesAvailable) {
      parts[taken++] = order[at];
      bytesAvailable -= (long)order[at]->size + 2;
    }
  }

  order.release();

  return taken;
}

/** After a send: told once more, and gone if told often enough. */
static void toldOnce(swim_t *self, const char *const *hosts, size_t count) {

  for (size_t i = 0; i < count; ++i) {

    for (size_t at = 0; at < self->updates.count; ++at) {

      update_t *u = &self->updates[at];

      if (strcmp(u->host, hosts[i]) != 0)
        continue;

      u->attempts += 1;
      u->order = self->order++;

      if (u->attempts >= self->disseminationLimit) {
        free(u->bytes);
        self->updates[at] = self->updates[self->updates.count - 1];
        self->updates.count -= 1;
      }

      break;
    }
  }
}

/* ------------------------------------------------------------------- net */

static void sendBuffer(swim_t *self, const char *host, const uint8_t *data,
                       size_t size) {

  if (self->hooks.send != NULL)
    self->hooks.send(self->hooks.context, host, data, size);
}

/** [0][u16le count] then [u16le length][message]... */
static void compound(mp_buf_t *out, const uint8_t *const *parts,
                     const size_t *sizes, size_t count) {

  out->reset();
  out->byte(SWIM_COMPOUND);
  out->u16le((uint16_t)count);

  for (size_t at = 0; at < count; ++at) {
    out->u16le((uint16_t)sizes[at]);
    out->raw(parts[at], sizes[at]);
  }
}

/**
 * Net.sendMessage: one message, with as many updates piggybacked as fit in
 * maxDgramSize - 1 - 2 * 2 - its own length. Without any, it goes alone.
 */
static void sendMessage(swim_t *self, const char *host, const uint8_t *message,
                        size_t size) {

  enum { ROOM = 256 };
  const update_t *updates[ROOM];
  const uint8_t *parts[ROOM + 1];
  size_t sizes[ROOM + 1];
  const char *hosts[ROOM];
  char told[ROOM][SWIM_HOST_MAX];
  long available = (long)self->options.maxDgramSize - 1 - 2 * 2 - (long)size;
  size_t count = takeUpdates(self, available, updates, ROOM);
  mp_buf_t out = {0};

  if (count == 0) {
    sendBuffer(self, host, message, size);
    return;
  }

  parts[0] = message;
  sizes[0] = size;

  for (size_t at = 0; at < count; ++at) {
    parts[at + 1] = updates[at]->bytes;
    sizes[at + 1] = updates[at]->size;
    memcpy(told[at], updates[at]->host, SWIM_HOST_MAX);
    hosts[at] = told[at];
  }

  compound(&out, parts, sizes, count + 1);

  if (!out.failed)
    sendBuffer(self, host, out.data, out.len);

  out.release();
  toldOnce(self, hosts, count);
}

/** A message of one type with a msgpack body built by `body`. */
static void sendTyped(swim_t *self, const char *host, int type,
                      const mp_buf_t *body) {

  mp_buf_t message = {0};

  message.byte((uint8_t)type);
  message.raw(body->data, body->len);

  if (!message.failed)
    sendMessage(self, host, message.data, message.len);

  message.release();
}

/**
 * Net.sendMessages: many messages packed into compounds of at most
 * maxDgramSize, nothing piggybacked - a Sync and its answer go this way.
 *
 * Kept as the JS has it, including its count: after the first compound is
 * flushed the room starts at maxDgramSize - 2, forgetting the type byte. One
 * message too big for a datagram on its own is dropped (swim-ng logs it).
 */
static void sendMessages(swim_t *self, const char *host,
                         const mp_buf_t *messages, size_t count) {

  long available = (long)self->options.maxDgramSize - 1 - 2;
  const uint8_t **parts = (const uint8_t **)malloc((count + 1) * sizeof *parts);
  size_t *sizes = (size_t *)malloc((count + 1) * sizeof *sizes);
  size_t held = 0;
  mp_buf_t out = {0};

  if (parts == NULL || sizes == NULL) {
    free(parts);
    free(sizes);
    return;
  }

  for (size_t i = 0; i < count; ++i) {

    long size = (long)messages[i].len;

    if (size + 2 < available) {
      parts[held] = messages[i].data;
      sizes[held] = messages[i].len;
      held += 1;
      available -= size + 2;
    } else if (held == 0) {
      /* oversized, and dropped */
    } else {
      compound(&out, parts, sizes, held);
      sendBuffer(self, host, out.data, out.len);
      available = (long)self->options.maxDgramSize - 2;
      held = 0;
      i -= 1;
    }
  }

  if (held > 0) {
    compound(&out, parts, sizes, held);
    sendBuffer(self, host, out.data, out.len);
  }

  out.release();
  free(parts);
  free(sizes);
}

static void sendSeq(swim_t *self, const char *host, int type, int64_t seq,
                    const char *destination) {

  mp_buf_t body = {0};

  body.map(destination != NULL ? 2 : 1);
  body.cstr("seq");
  body.integer(seq);

  if (destination != NULL) {
    body.cstr("destination");
    body.cstr(destination);
  }

  sendTyped(self, host, type, &body);
  body.release();
}

static void sendUpdate(swim_t *self, const char *host, const swim_member_t *m) {

  mp_buf_t body = {0};

  writeData(&body, m);
  sendTyped(self, host, SWIM_UPDATE, &body);
  body.release();
}

/* -------------------------------------------------------------- membership */

static void emit(swim_t *self, swim_event_t event, const swim_member_t *m) {

  if (event == SWIM_EVENT_CHANGE)
    updateDisseminationLimit(self);

  if (event == SWIM_EVENT_UPDATE && m != NULL)
    addUpdate(self, m);

  if (self->hooks.event != NULL)
    self->hooks.event(self->hooks.context, event, m);
}

static int isLocal(swim_t *self, const char *host) {
  return strcmp(self->local.host, host) == 0;
}

static swim_member_t *memberOf(swim_t *self, const char *host) {

  swim_member_t **m = self->members.get(host);

  return m != NULL ? *m : NULL;
}

static swim_member_t *faultyOf(swim_t *self, const char *host) {

  swim_member_t **m = self->faulty.get(host);

  return m != NULL ? *m : NULL;
}

static void iterableRemove(swim_t *self, const char *host) {

  for (size_t at = 0; at < self->iterable.count; ++at) {

    if (strcmp(self->iterable[at].host, host) == 0) {
      self->iterable[at] = self->iterable[self->iterable.count - 1];
      self->iterable.count -= 1;
      return;
    }
  }
}

static void iterableAdd(swim_t *self, const char *host) {

  hostName_t h;

  iterableRemove(self, host);
  copyHost(h.host, host, strlen(host));
  self->iterable.push(h);
}

static void freeMember(swim_member_t *m) {

  if (m == NULL)
    return;

  free(m->meta);
  free(m);
}

static void forgetFaulty(swim_t *self, const char *host) {

  swim_member_t *f = faultyOf(self, host);

  if (f != NULL) {
    self->faulty.remove(f->host);
    freeMember(f);
  }
}

/**
 * `new Member(data)` into hostToMember, emitting Change when it is new.
 * Answers whether it is there now; the table owns it, ask memberOf for it.
 */
static int storeMember(swim_t *self, const swim_member_t *data) {

  swim_member_t *m = memberOf(self, data->host);
  int fresh = m == NULL;

  if (fresh) {

    m = (swim_member_t *)calloc(1, sizeof *m);

    if (m == NULL)
      return 0;

    memcpy(m->host, data->host, SWIM_HOST_MAX);
    self->members.set(m->host, m);
    iterableAdd(self, m->host);
  }

  setMeta(m, data->meta, data->metaLen);
  m->state = data->state;
  m->incarnation = data->incarnation;

  if (fresh)
    emit(self, SWIM_EVENT_CHANGE, m);

  return 1;
}

/**
 * Member.incarnate for our own member. preferCurrentMeta is on (microwizard
 * sets it), so our meta never changes because somebody else said so.
 */
static int incarnate(swim_t *self, const swim_member_t *data, int force) {

  if (data->incarnation > self->local.incarnation) {
    self->local.incarnation = data->incarnation + 1;
    return 1;
  }

  if (data->incarnation == self->local.incarnation && force) {
    self->local.incarnation = data->incarnation + 1;
    return 1;
  }

  return 0;
}

static void updateAlive(swim_t *self, const swim_member_t *data) {

  swim_member_t *m;
  swim_member_t *f;

  if (isLocal(self, data->host)) {
    if (incarnate(self, data, 0))
      emit(self, SWIM_EVENT_UPDATE, &self->local);
    return;
  }

  f = faultyOf(self, data->host);

  if (f != NULL && f->incarnation >= data->incarnation)
    return;

  m = memberOf(self, data->host);

  /* strictly newer: an alive at the same incarnation does not lift a suspect */
  if (m != NULL && data->incarnation <= m->incarnation)
    return;

  timersClear(self, TIMER_SUSPECT, 0, data->host);
  forgetFaulty(self, data->host);

  if (storeMember(self, data))
    emit(self, SWIM_EVENT_UPDATE, memberOf(self, data->host));
}

static void updateSuspect(swim_t *self, const swim_member_t *data) {

  swim_member_t *m;
  swim_member_t *f;

  /* about us: refute, by force - and say so even when nothing changed */
  if (isLocal(self, data->host)) {
    incarnate(self, data, 1);
    emit(self, SWIM_EVENT_UPDATE, &self->local);
    return;
  }

  f = faultyOf(self, data->host);

  if (f != NULL && f->incarnation >= data->incarnation)
    return;

  m = memberOf(self, data->host);

  if (m != NULL && !(data->incarnation > m->incarnation ||
                     (data->incarnation == m->incarnation &&
                      m->state == SWIM_ALIVE)))
    return;

  /**
   * No suspect timer here. Only the node whose own pings failed starts one;
   * everyone else just writes the state down and waits for its faulty.
   */
  forgetFaulty(self, data->host);

  if (storeMember(self, data))
    emit(self, SWIM_EVENT_UPDATE, memberOf(self, data->host));
}

static void updateFaulty(swim_t *self, const swim_member_t *data) {

  swim_member_t *m;
  swim_member_t *f;

  if (isLocal(self, data->host)) {
    incarnate(self, data, 1);
    emit(self, SWIM_EVENT_UPDATE, &self->local);
    return;
  }

  m = memberOf(self, data->host);

  /* a faulty about somebody we do not know is dropped, not remembered */
  if (m == NULL || data->incarnation < m->incarnation)
    return;

  forgetFaulty(self, data->host);

  /* the faulty record first: data->meta may be the member's own buffer */
  f = (swim_member_t *)calloc(1, sizeof *f);

  if (f != NULL) {
    memcpy(f->host, data->host, SWIM_HOST_MAX);
    setMeta(f, data->meta, data->metaLen);
    f->state = SWIM_FAULTY;
    f->incarnation = data->incarnation;
  }

  self->members.remove(m->host);
  iterableRemove(self, m->host);
  freeMember(m);

  if (f == NULL)
    return;

  self->faulty.set(f->host, f);

  emit(self, SWIM_EVENT_CHANGE, f);
  emit(self, SWIM_EVENT_UPDATE, f);
}

static void onUpdate(swim_t *self, const swim_member_t *data) {

  switch (data->state) {
  case SWIM_ALIVE:
    updateAlive(self, data);
    break;
  case SWIM_SUSPECT:
    updateSuspect(self, data);
    break;
  case SWIM_FAULTY:
    updateFaulty(self, data);
    break;
  }
}

/** FailureDetector's Suspect event, handled by Membership.onSuspect. */
static void onSuspect(swim_t *self, const char *host, int64_t incarnation,
                      long now) {

  swim_member_t *m = memberOf(self, host);
  swim_member_t data;

  /**
   * The JS hands over the member object it pinged. If that member has moved
   * on - gone faulty, or a newer incarnation - the suspect update it builds
   * is dropped by updateSuspect anyway; so there is nothing to do.
   */
  if (m == NULL || m->incarnation != incarnation)
    return;

  if (hasTimer(self, TIMER_SUSPECT, host))
    return;

  timerAdd(self, now + self->options.suspectTimeout, TIMER_SUSPECT, 0, 0, host,
           incarnation);

  data = *m;
  data.state = SWIM_SUSPECT;
  onUpdate(self, &data);
}

typedef mp_buf_t[] bufs_t;

/** One Update for a member, at the end of `list`. */
static void addData(bufs_t *list, const swim_member_t *m) {

  mp_buf_t one = {0};

  one.byte(SWIM_UPDATE);
  writeData(&one, m);

  if (!list->push(one))
    one.release();
}

/** all(hasLocal, hasFaulty): members, then us, then the faulty ones. */
static void allData(swim_t *self, int withLocal, int withFaulty, bufs_t *list) {

  for (entry in self->members)
    addData(list, entry->value);

  if (withLocal)
    addData(list, &self->local);

  if (withFaulty)
    for (entry in self->faulty)
      addData(list, entry->value);
}

static void freeList(bufs_t *list) {

  for (b in *list)
    b->release();

  list->release();
}

/** Membership.onSync: take the sender in, then tell it everything we know. */
static void onSync(swim_t *self, const swim_member_t *data) {

  bufs_t list;

  updateAlive(self, data);

  allData(self, 1, 1, &list);
  sendMessages(self, data->host, list.items, list.count);
  freeList(&list);
}

/** Membership.sync: a Sync with us, and an Update for everyone else we know. */
static void syncTo(swim_t *self, const char *const *hosts, int hostCount) {

  bufs_t list;
  mp_buf_t sync = {0};

  sync.byte(SWIM_SYNC);
  writeData(&sync, &self->local);

  if (list.push(sync)) {

    allData(self, 0, 1, &list);

    for (int h = 0; h < hostCount; ++h)
      sendMessages(self, hosts[h], list.items, list.count);
  } else
    sync.release();

  freeList(&list);
}

/* --------------------------------------------------------- failure detector */

/** Membership.next: a random one of those not pinged yet this round. */
static swim_member_t *nextMember(swim_t *self) {

  size_t pick;
  hostName_t chosen;

  if (self->iterable.count == 0) {
    for (entry in self->members)
      iterableAdd(self, entry->key);
  }

  if (self->iterable.count == 0)
    return NULL;

  pick = (size_t)rand() % self->iterable.count;
  chosen = self->iterable[pick];
  iterableRemove(self, chosen.host);

  return memberOf(self, chosen.host);
}

static void ping(swim_t *self, long now) {

  swim_member_t *m = nextMember(self);
  uint32_t seq;

  if (m == NULL)
    return;

  seq = self->seq++;
  timerAdd(self, now + self->options.pingTimeout, TIMER_PING, seq, 0, m->host,
           m->incarnation);
  sendSeq(self, m->host, SWIM_PING, seq, NULL);
}

/**
 * Membership.random(n): up to n members, drawn without repeats. The member
 * being checked can be one of them - swim-ng does not leave it out, so it may
 * be asked to ping itself.
 */
static void pingReq(swim_t *self, const char *host, int64_t incarnation,
                    long now) {

  size_t count = self->members.count;
  size_t want = (size_t)self->options.pingReqGroupSize;
  const char *[] hosts;
  uint32_t group;

  if (count == 0)
    return;

  for (entry in self->members)
    if (!hosts.push(entry->key)) {
      hosts.release();
      return;
    }

  group = ++self->groups;
  timerAdd(self, now + self->options.pingReqTimeout, TIMER_PING_REQ, 0, group,
           host, incarnation);

  for (size_t i = 0; i < want && i < count; ++i) {

    size_t index = i + (size_t)rand() % (count - i);
    const char *relay = hosts[index];
    uint32_t seq = self->seq++;
    callback_t cb;

    hosts[index] = hosts[i];

    memset(&cb, 0, sizeof cb);
    cb.kind = CALLBACK_RELAYED;
    cb.group = group;
    self->callbacks.set(seq, cb);

    timerAdd(self, now + self->options.pingReqTimeout, TIMER_SEQ, seq, 0, NULL, 0);
    sendSeq(self, relay, SWIM_PING_REQ, seq, host);
  }

  hosts.release();
}

static void clearSeq(swim_t *self, uint32_t seq) {

  timersClearSeq(self, seq);
  self->callbacks.remove(seq);
}

static void onPing(swim_t *self, int64_t seq, const char *from) {
  sendSeq(self, from, SWIM_ACK, seq, NULL);
}

static void onPingReq(swim_t *self, int64_t seq, const char *destination,
                      const char *from, long now) {

  uint32_t ours = self->seq++;
  callback_t cb;

  memset(&cb, 0, sizeof cb);
  cb.kind = CALLBACK_FORWARD;
  cb.seq = seq;
  copyHost(cb.host, from, strlen(from));
  self->callbacks.set(ours, cb);

  timerAdd(self, now + self->options.pingTimeout, TIMER_SEQ, ours, 0, NULL, 0);
  sendSeq(self, destination, SWIM_PING, ours, NULL);
}

static void onAck(swim_t *self, int64_t seq, const char *from) {

  callback_t *found = seq >= 0 ? self->callbacks.get((size_t)seq) : NULL;
  swim_member_t *m;

  if (found != NULL) {

    callback_t cb = *found;

    clearSeq(self, (uint32_t)seq);

    if (cb.kind == CALLBACK_RELAYED)
      timersClear(self, TIMER_PING_REQ, cb.group, NULL);
    else
      sendSeq(self, cb.host, SWIM_ACK, cb.seq, NULL);

  } else if (seq >= 0)
    clearSeq(self, (uint32_t)seq);

  /**
   * Membership.onAck: an ack from a member we hold as suspect gets told so,
   * which is how it learns to refute. Keyed by where the ack came from.
   */
  m = memberOf(self, from);

  if (m != NULL && m->state == SWIM_SUSPECT)
    sendUpdate(self, from, m);
}

/* ------------------------------------------------------------------- input */

/** Reads a Sync or Update body into `into`; meta points into the body. */
static int readData(const uint8_t *body, size_t size, swim_member_t *into) {

  mp_reader_t r = mp_reader(body, size);
  uint32_t count;
  int haveHost = 0;
  int haveState = 0;

  memset(into, 0, sizeof *into);

  if (!r.map(&count))
    return 0;

  while (count-- > 0) {

    if (r.key("meta")) {

      size_t from = r.at;

      if (r.peek() == MP_EXT || r.peek() == MP_NIL)
        r.skip(); /* undefined, or nil: no meta */
      else {
        if (!r.skip())
          return 0;
        into->meta = (uint8_t *)(body + from);
        into->metaLen = r.at - from;
      }

    } else if (r.key("host")) {

      const char *text;
      uint32_t length;

      if (!r.str(&text, &length) || length >= SWIM_HOST_MAX)
        return 0;

      copyHost(into->host, text, length);
      haveHost = 1;

    } else if (r.key("state")) {

      int64_t state;

      if (!r.integer(&state) || state < 0 || state > 2)
        return 0;

      into->state = (swim_state_t)state;
      haveState = 1;

    } else if (r.key("incarnation")) {

      if (!r.integer(&into->incarnation))
        return 0;

    } else if (!r.skip() || !r.skip()) {
      return 0;
    }
  }

  return r.error == NULL && haveHost && haveState;
}

static int readSeq(const uint8_t *body, size_t size, int64_t *seq,
                   char *destination) {

  mp_reader_t r = mp_reader(body, size);
  uint32_t count;
  int haveSeq = 0;

  if (destination != NULL)
    destination[0] = 0;

  if (!r.map(&count))
    return 0;

  while (count-- > 0) {

    if (r.key("seq")) {
      if (!r.integer(seq))
        return 0;
      haveSeq = 1;
    } else if (destination != NULL && r.key("destination")) {

      const char *text;
      uint32_t length;

      if (!r.str(&text, &length) || length >= SWIM_HOST_MAX)
        return 0;

      copyHost(destination, text, length);

    } else if (!r.skip() || !r.skip()) {
      return 0;
    }
  }

  return r.error == NULL && haveSeq &&
         (destination == NULL || destination[0] != 0);
}

typedef struct {
  swim_t *self;
  const char *from;
  long now;
} delivery_t;

static void deliver(void *context, int type, const uint8_t *body, size_t size) {

  delivery_t *d = (delivery_t *)context;
  swim_t *self = d->self;
  swim_member_t data;
  int64_t seq;
  char destination[SWIM_HOST_MAX];

  switch (type) {
  case SWIM_PING:
    if (readSeq(body, size, &seq, NULL))
      onPing(self, seq, d->from);
    break;
  case SWIM_PING_REQ:
    if (readSeq(body, size, &seq, destination))
      onPingReq(self, seq, destination, d->from, d->now);
    break;
  case SWIM_ACK:
    if (readSeq(body, size, &seq, NULL))
      onAck(self, seq, d->from);
    break;
  case SWIM_SYNC:
    if (readData(body, size, &data))
      onSync(self, &data);
    break;
  case SWIM_UPDATE:
    if (readData(body, size, &data))
      onUpdate(self, &data);
    break;
  default:
    break; /* Compressed, Encrypted and anything else: swim-ng ignores them */
  }
}

static int framesOf(const uint8_t *data, size_t size,
                    void (*each)(void *, int, const uint8_t *, size_t),
                    void *context, int depth) {

  int found = 0;

  if (size < 1 || depth > 4)
    return -1;

  if (data[0] != SWIM_COMPOUND) {
    each(context, data[0], data + 1, size - 1);
    return 1;
  }

  {
    size_t count;
    size_t at = 3;

    if (size < 3)
      return -1;

    count = (size_t)data[1] | (size_t)data[2] << 8;

    for (size_t i = 0; i < count; ++i) {

      size_t length;
      int inner;

      /* as swim-ng: a part whose length does not fit ends the reading */
      if (size - at < 2)
        break;

      length = (size_t)data[at] | (size_t)data[at + 1] << 8;
      at += 2;

      if (length > size - at)
        length = size - at;

      inner = framesOf(data + at, length, each, context, depth + 1);

      if (inner > 0)
        found += inner;

      at += length;
    }
  }

  return found;
}

int swim_frames(const uint8_t *data, size_t size,
                void (*each)(void *context, int type, const uint8_t *body,
                             size_t bodySize),
                void *context) {
  return framesOf(data, size, each, context, 0);
}

void swim_receive(swim_t *self, const uint8_t *data, size_t size,
                  const char *from, long now) {

  delivery_t d;

  d.self = self;
  d.from = from;
  d.now = now;

  framesOf(data, size, deliver, &d, 0);
}

/* ---------------------------------------------------------------- the rest */

static int joinedCount(swim_t *self) {

  int responded = 0;

  for (at in self->joining)
    responded += memberOf(self, at->host) != NULL;

  return responded;
}

static void fire(swim_t *self, swim_timer_t t, long now) {

  switch (t.kind) {
  case TIMER_TICK:
    ping(self, now);
    timerAdd(self, t.when + self->options.interval, TIMER_TICK, 0, 0, NULL, 0);
    break;
  case TIMER_PING:
    clearSeq(self, t.seq);
    pingReq(self, t.host, t.incarnation, now);
    break;
  case TIMER_SEQ:
    self->callbacks.remove(t.seq);
    break;
  case TIMER_PING_REQ:
    onSuspect(self, t.host, t.incarnation, now);
    break;
  case TIMER_SUSPECT: {
    /* the suspect as it was set, now faulty - at the incarnation it had then */
    swim_member_t *m = memberOf(self, t.host);
    swim_member_t data;

    memset(&data, 0, sizeof data);
    copyHost(data.host, t.host, strlen(t.host));

    if (m != NULL) {
      data.meta = m->meta;
      data.metaLen = m->metaLen;
    }

    data.state = SWIM_FAULTY;
    data.incarnation = t.incarnation;
    onUpdate(self, &data);
    break;
  }
  case TIMER_JOIN_CHECK:
    if (!self->joinPending)
      break;
    if (joinedCount(self) >= 1) {
      self->joinPending = 0;
      timersClear(self, TIMER_JOIN_TIMEOUT, 0, NULL);
      emit(self, SWIM_EVENT_JOINED, NULL);
    } else
      timerAdd(self, now + self->options.joinCheckInterval, TIMER_JOIN_CHECK,
               0, 0, NULL, 0);
    break;
  case TIMER_JOIN_TIMEOUT:
    if (!self->joinPending)
      break;
    self->joinPending = 0;
    timersClear(self, TIMER_JOIN_CHECK, 0, NULL);
    emit(self, joinedCount(self) > 0 ? SWIM_EVENT_JOINED : SWIM_EVENT_JOIN_FAILED,
         NULL);
    break;
  }
}

void swim_advance(swim_t *self, long now) {

  for (;;) {

    size_t due = self->timers.count;
    swim_timer_t t;

    for (size_t at = 0; at < self->timers.count; ++at)
      if (self->timers[at].when <= now &&
          (due == self->timers.count ||
           self->timers[at].when < self->timers[due].when))
        due = at;

    if (due == self->timers.count)
      return;

    t = self->timers[due];
    timerDrop(self, due);
    fire(self, t, now);
  }
}

swim_t *swim_new(const swim_options_t *options, const char *host,
                 const uint8_t *meta, size_t metaLen, int64_t incarnation,
                 swim_hooks_t hooks) {

  swim_t *self = (swim_t *)calloc(1, sizeof *self);

  if (self == NULL)
    return NULL;

  if (options != NULL)
    self->options = *options;
  else
    swim_defaults(&self->options);

  self->hooks = hooks;
  copyHost(self->local.host, host, strlen(host));
  setMeta(&self->local, meta, metaLen);
  self->local.state = SWIM_ALIVE;
  self->local.incarnation = incarnation;

  updateDisseminationLimit(self);

  return self;
}

void swim_start(swim_t *self, long now) {

  if (self->started)
    return;

  self->started = 1;

  /* setImmediate for the first ping, then setInterval */
  timerAdd(self, now, TIMER_TICK, 0, 0, NULL, 0);
}

void swim_join(swim_t *self, const char *const *hosts, int count, long now) {

  const char *[] others;

  self->joining.release();
  timersClear(self, TIMER_JOIN_CHECK, 0, NULL);
  timersClear(self, TIMER_JOIN_TIMEOUT, 0, NULL);

  for (int i = 0; i < count; ++i) {

    hostName_t h;

    if (isLocal(self, hosts[i]))
      continue;

    copyHost(h.host, hosts[i], strlen(hosts[i]));
    self->joining.push(h);
    others.push(hosts[i]);
  }

  if (others.count == 0) {
    others.release();
    self->joinPending = 0;
    emit(self, SWIM_EVENT_JOINED, NULL);
    return;
  }

  syncTo(self, others.items, (int)others.count);
  others.release();

  self->joinPending = 1;
  timerAdd(self, now + self->options.joinTimeout, TIMER_JOIN_TIMEOUT, 0, 0, NULL, 0);

  if (self->options.joinCheckInterval < self->options.joinTimeout)
    timerAdd(self, now + self->options.joinCheckInterval, TIMER_JOIN_CHECK, 0,
             0, NULL, 0);
}

const swim_member_t *swim_local(const swim_t *self) { return &self->local; }

const swim_member_t *swim_get(const swim_t *self, const char *host) {
  return memberOf((swim_t *)self, host);
}

size_t swim_size(const swim_t *self) { return self->members.count; }

void swim_each(const swim_t *self,
               void (*each)(void *context, const swim_member_t *member),
               void *context) {

  swim_t *writable = (swim_t *)self;

  for (entry in writable->members)
    each(context, entry->value);
}

void swim_updateMeta(swim_t *self, const uint8_t *meta, size_t metaLen) {

  setMeta(&self->local, meta, metaLen);
  self->local.incarnation += 1;
  emit(self, SWIM_EVENT_UPDATE, &self->local);
}

void swim_free(swim_t *self) {

  if (self == NULL)
    return;

  for (entry in self->members)
    freeMember(entry->value);

  for (entry in self->faulty)
    freeMember(entry->value);

  for (u in self->updates)
    free(u->bytes);

  self->members.release();
  self->faulty.release();
  self->iterable.release();
  self->timers.release();
  self->callbacks.release();
  self->updates.release();
  self->joining.release();
  self->scratch.release();
  free(self->local.meta);
  free(self);
}
