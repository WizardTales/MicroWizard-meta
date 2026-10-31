/**
 * SWIM membership as @wzrdtales/swim-ng 0.6.2 runs it under sneeze-ng 0.13.0,
 * on the wire and in its decisions.
 *
 * Plain C, read by meta and by the C compiler; swim.c is the implementation
 * and docs/protokoll.md the description. This is the core alone: no socket,
 * no clock. Bytes come in through swim_receive, time through swim_advance,
 * and what has to go out goes to the send callback as finished datagrams
 * with the "ip:port" they are for. main.c is the driver that owns the UDP
 * socket and the timer; a test can be one too, with a fake clock.
 *
 * Three decisions worth knowing before reading on:
 *
 *   - A member is keyed by the "ip:port" string it announces (`host`), and
 *     everything addressed to a member goes there. Only Ack (to a Ping) and
 *     the forwarded Ack (to a PingReq) go to the address the datagram came
 *     from, as in swim-ng.
 *   - A member's meta is kept as the msgpack bytes it came in, and passed on
 *     as those bytes. swim never looks inside; the mesh layer does.
 *   - An incarnation is an int64 here and a JS number on the other side, so
 *     it goes out with the JS number rule - Date.now() as a float64.
 */
#ifndef MWM_SWIM_H
#define MWM_SWIM_H

#include <stddef.h>
#include <stdint.h>

#define SWIM_HOST_MAX 64

/** Message types: the first byte of every datagram and compound part. */
enum {
  SWIM_COMPOUND = 0,
  SWIM_COMPRESSED = 1,
  SWIM_ENCRYPTED = 2,
  SWIM_PING = 10,
  SWIM_PING_REQ = 11,
  SWIM_SYNC = 12,
  SWIM_ACK = 13,
  SWIM_UPDATE = 14
};

typedef enum { SWIM_ALIVE = 0, SWIM_SUSPECT = 1, SWIM_FAULTY = 2 } swim_state_t;

typedef struct {
  char host[SWIM_HOST_MAX];

  /** The member's meta as msgpack, or NULL when it came without one. */
  uint8_t *meta;
  size_t metaLen;

  swim_state_t state;
  int64_t incarnation;
} swim_member_t;

/** sneeze-ng's values, which is what every node in the mesh runs with. */
typedef struct {
  long interval;            /* 333: one ping per interval */
  long pingTimeout;         /* 444: then ping-req through others */
  long pingReqTimeout;      /* 555: then suspect */
  int pingReqGroupSize;     /* 7 */
  long suspectTimeout;      /* 999: then faulty */
  long joinTimeout;         /* 999 */
  long joinCheckInterval;   /* 50, swim-ng's default; sneeze does not set it */
  int disseminationFactor;  /* 22 */
  size_t maxDgramSize;      /* 2048 */
} swim_options_t;

void swim_defaults(swim_options_t *into);

typedef enum {
  /** A member turned up or was declared faulty (swim-ng's Change). */
  SWIM_EVENT_CHANGE,

  /** Any accepted update, our own included (swim-ng's Update). */
  SWIM_EVENT_UPDATE,

  /** At least one of the hosts handed to swim_join answered. */
  SWIM_EVENT_JOINED,

  /** None did within joinTimeout (swim-ng's JoinFailedError). */
  SWIM_EVENT_JOIN_FAILED
} swim_event_t;

typedef struct {
  void *context;

  /** One finished datagram for `host`, "ip:port". */
  void (*send)(void *context, const char *host, const uint8_t *data,
               size_t size);

  /** `member` is only good for the length of the call. NULL for the join events. */
  void (*event)(void *context, swim_event_t event, const swim_member_t *member);
} swim_hooks_t;

typedef struct swim_t swim_t;

/**
 * A member that is us: `host` as the others will address us, our meta as
 * msgpack (copied), and the incarnation we start at - Date.now() in sneeze,
 * which is what makes a restart on the same host:port win over its own
 * faulty record on everyone else.
 */
swim_t *swim_new(const swim_options_t *options, const char *host,
                 const uint8_t *meta, size_t metaLen, int64_t incarnation,
                 swim_hooks_t hooks);

void swim_free(swim_t *self);

/** Starts the failure detector: the first ping goes out on the next advance. */
void swim_start(swim_t *self, long now);

/**
 * Sync to these hosts (our own host is left out), and report JOINED or
 * JOIN_FAILED. An empty list is joined at once, as a lone base is.
 */
void swim_join(swim_t *self, const char *const *hosts, int count, long now);

/** One datagram as it came off the socket, from "ip:port". */
void swim_receive(swim_t *self, const uint8_t *data, size_t size,
                  const char *from, long now);

/** Runs whatever is due at `now`. */
void swim_advance(swim_t *self, long now);

/** When the next thing is due, or -1 when nothing is. */
long swim_nextDeadline(const swim_t *self);

/** Our own member, as the others hear about it. */
const swim_member_t *swim_local(const swim_t *self);

/** An alive or suspect member, or NULL. */
const swim_member_t *swim_get(const swim_t *self, const char *host);

/** How many alive and suspect members, not counting us. */
size_t swim_size(const swim_t *self);

/** Calls `each` for every alive and suspect member. */
void swim_each(const swim_t *self, void (*each)(void *context,
                                               const swim_member_t *member),
               void *context);

/** New meta for us: the incarnation goes up and the update goes round. */
void swim_updateMeta(swim_t *self, const uint8_t *meta, size_t metaLen);

/**
 * A datagram taken apart into its messages, for a test or a capture: calls
 * `each` with every message's type and msgpack body, compounds opened.
 * Returns how many there were, or -1 if the framing did not hold.
 */
int swim_frames(const uint8_t *data, size_t size,
                void (*each)(void *context, int type, const uint8_t *body,
                             size_t bodySize),
                void *context);

#endif
