/**
 * The client against servers that misbehave on purpose, in one process.
 *
 * test/harness/client.js is the proof against real Node services through a
 * real mesh; this is where every way a server can answer is made to happen
 * on demand: answers, errors, overload, silence, a port nobody listens on,
 * lines that are not answers at all, a keepalive nobody returns. The servers
 * are meta TCP listeners on 127.0.0.1 in the 389xx range, the members are
 * handed to the client the way mesh.c hands them over - msgpack meta in the
 * shape microwizard announces.
 *
 *   client_test [fixtures] --no-patient  everything as microwizard 1.6.2 does it
 *   client_test [fixtures]               the patient option, the default
 *   client_test [fixtures] --short       patient with a request timeout of
 *                                        400 ms, as test/balance.test.js of
 *                                        fix/consume-wait has it
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <meta_json.h>
#include <meta_tasks.h>
#include <meta_tcp.h>
#include <meta_time.h>

#include "client.h"
#include "msgpack.h"
#include "service.h"

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

#define PORT_BASE 38900
#define NOBODY_PORT 38919
#define LONGEST 65536

/* ------------------------------------------------------- fake servers */

typedef enum {
  ECHO,      /* answers {echo: args, from: name} */
  OVER,      /* error with details.message retry_later_err_overload */
  THROW,     /* error {message, code} */
  MUTE,      /* reads, never answers */
  JUNK,      /* every unusable line there is, then the answer */
  DEAF,      /* answers calls, ignores {"h":1} */
  ONCE_OVER, /* overloaded for the first call, then echo */
  CLOSING    /* error.message retry_later_err_overload, no details */
} mode_t_;

/** What a server saw, for the test to look at: one note per thing. */
typedef enum { N_ACCEPT, N_LINE, N_END } note_kind_t;

typedef struct {
  int server;
  note_kind_t kind;
  char *text;
} note_t;

static @chan(note_t, 4096) notes;

task servers;

static char *answerTo(json_t asked, int server, mode_t_ mode, int calls) {

  char *id = asked.id.owned();
  char *args = asked.args.isNothing() ? strdup("null")
                                             : asked.args.owned();
  size_t room = strlen(id) + strlen(args) + 256;
  char *out = malloc(room);

  if (mode == OVER || (mode == ONCE_OVER && calls == 1))
    snprintf(out, room,
             "{\"id\":%s,\"k\":\"res\",\"sync\":true,\"error\":{\"message\":"
             "\"overloaded\",\"name\":\"Error\",\"details\":{\"message\":"
             "\"retry_later_err_overload\"}}}",
             id);
  else if (mode == CLOSING)
    snprintf(out, room,
             "{\"id\":%s,\"k\":\"res\",\"sync\":true,\"error\":{\"message\":"
             "\"retry_later_err_overload\",\"name\":\"Error\"}}",
             id);
  else if (mode == THROW)
    snprintf(out, room,
             "{\"id\":%s,\"k\":\"res\",\"sync\":true,\"error\":{\"message\":"
             "\"thrown on purpose\",\"code\":\"E_PURPOSE\",\"name\":\"Error\"}}",
             id);
  else
    snprintf(out, room,
             "{\"id\":%s,\"k\":\"res\",\"sync\":true,\"res\":{\"code\":200,"
             "\"msg\":{\"echo\":%s,\"from\":\"s%d\"}}}",
             id, args, server);

  free(args);
  free(id);

  return out;
}

static void fakeConn(tcp_t *peer, int server, mode_t_ mode) {

  int calls = 0;

  notes.send((note_t){server, N_ACCEPT, NULL});

  for (;;) {

    const char *text = peer.line(LONGEST);
    json_t asked;

    if (text == NULL)
      break;

    asked = meta_toJSON(text);
    notes.send((note_t){server, N_LINE, strdup(text)});

    if (asked.h.number() == 1) {
      if (mode != DEAF)
        peer.writeLine("{\"h\":2}");
    } else if (mode != MUTE) {

      char *out;

      ++calls;
      out = answerTo(asked, server, mode, calls);

      if (mode == JUNK) {
        char *id = asked.id.owned();
        char line[256];

        peer.writeLine("this is not json");
        peer.writeLine("null");
        peer.writeLine("5");
        peer.writeLine("\"text\"");
        peer.writeLine("[1,2]");
        peer.writeLine("{\"k\":\"nope\",\"id\":\"x\"}");
        peer.writeLine("{\"k\":\"res\",\"id\":null,\"res\":1}");
        peer.writeLine("{\"k\":\"res\",\"id\":\"nobody-asked\",\"res\":1}");
        peer.writeLine("{\"k\":\"res\",\"id\":12345,\"res\":1}");
        snprintf(line, sizeof line, "{\"id\":%s,\"k\":\"res\",\"sync\":false,\"res\":\"async\"}", id);
        peer.writeLine(line);
        free(id);
      }

      peer.writeLine(out);
      free(out);
    }

    asked.release();
  }

  notes.send((note_t){server, N_END, NULL});
  peer.close();
}

static void fakeServe(tcpListener_t *server, int index, mode_t_ mode) {

  for (;;) {

    tcp_t *peer = server.accept();

    if (peer.failed()) {
      peer.close();
      return;
    }

    go servers fakeConn(peer, index, mode);
  }
}

/* -------------------------------------------------- the notes, read */

typedef struct {
  int accepts;
  int lines;
  int ends;
  int heartbeats;
  char ids[16][32];
  int idCount;
  char last[1024];
} seen_t;

#define SERVERS 14

/** Drains whatever the servers said so far, waiting `ms` first: one seen_t each. */
static void lookAll(seen_t *all, long ms) {

  memset(all, 0, SERVERS * sizeof *all);
  meta_sleepMs(ms);

  for (;;) {

    note_t n = {0, N_ACCEPT, NULL};
    int got = 0;
    seen_t *seen;

    /* the arm's n is its own; it is copied out */
    select {
    case one in notes:
      n = one;
      got = 1;
    default:
      got = 0;
    }

    if (!got)
      break;

    seen = n.server >= 0 && n.server < SERVERS ? &all[n.server] : NULL;

    if (seen != NULL && n.kind == N_ACCEPT)
      seen->accepts += 1;
    else if (seen != NULL && n.kind == N_END)
      seen->ends += 1;
    else if (seen != NULL) {

      json_t line = meta_toJSON(n.text);

      if (line.h.number() == 1)
        seen->heartbeats += 1;
      else {
        seen->lines += 1;
        snprintf(seen->last, sizeof seen->last, "%s", n.text);
        if (seen->idCount < 16)
          snprintf(seen->ids[seen->idCount++], sizeof seen->ids[0], "%s",
                   line.id.text());
      }

      line.release();
    }

    free(n.text);
  }
}

/** The same for one server; the others' notes are dropped. */
static void look(seen_t *seen, int server, long ms) {

  seen_t all[SERVERS];

  lookAll(all, ms);
  *seen = all[server];
}

/* --------------------------------------------------- members, made */

/** A member announced as microwizard announces one listen. */
static void announce(int added, const char *identifier, const char *pin,
                     int port, long timeout) {

  mp_buf_t meta = {0};

  meta.map(5);
  meta.cstr("config");
  meta.map(8);
  meta.cstr("host");
  meta.cstr("127.0.0.1");
  meta.cstr("port");
  meta.integer(port);
  meta.cstr("timeout");
  meta.integer(timeout);
  meta.cstr("connectTimeout");
  meta.integer(355);
  meta.cstr("pin");
  meta.array(1);
  meta.cstr(pin);
  meta.cstr("model");
  meta.cstr("consume");
  meta.cstr("type");
  meta.cstr("tcp");
  meta.cstr("ismesh");
  meta.boolean(1);
  meta.cstr("instance");
  meta.cstr("0a0b0c0d0e0");
  meta.cstr("identifier$");
  meta.cstr(identifier);
  meta.cstr("tag$");
  meta.cstr("mc~t");
  meta.cstr("v$");
  meta.integer(0);

  mwm_clientMember(added, identifier, meta.data, meta.len);
  meta.release();
}

/* --------------------------------------------------------- the calls */

typedef struct {
  int rejected;
  char text[2048];
  long ms;
} outcome_t;

static void act(outcome_t *o, const char *pattern, const char *args) {

  json_t in = meta_toJSON(args);
  long began = meta_nowMs();
  mwm_settled_t got = mwm_act(pattern, in);
  char *text = got.value.isNothing()
                   ? strdup("undefined")
                   : yyjson_mut_val_write(got.value._node, 0, NULL);

  o->rejected = got.rejected;
  o->ms = meta_nowMs() - began;
  snprintf(o->text, sizeof o->text, "%s", text);
  free(text);
  got.value.release();
  in.release();
}

static int has(const outcome_t *o, const char *part) {
  return strstr(o->text, part) != NULL;
}

static mwm_answer_t localPing(json_t args) {
  json_t res = {who: "local role:lw,cmd:ping"};
  return (mwm_answer_t){0, res};
}

static mwm_answer_t localX(json_t args) {
  json_t res = {who: "local role:lx"};
  return (mwm_answer_t){0, res};
}

static mwm_answer_t localQ(json_t args) {
  json_t res = {who: "local role:dq,cmd:x"};
  return (mwm_answer_t){0, res};
}

/**
 * microwizard fix/dead-pin: the last target of role:dp,cmd:x gone (s6), the
 * call falls through to role:dp,cmd:* (s5); the member back, the pin wins
 * again. A handler of ours on the same pin is what is left of it.
 */
static void deadPin(void) {

  outcome_t o;
  seen_t seen;

  announce(1, "i14~role:dp,cmd:x~1", "role:dp,cmd:x", PORT_BASE + 6, 5555);
  announce(1, "i15~role:dp,cmd:*~1", "role:dp,cmd:*", PORT_BASE + 5, 5555);

  act(&o, "role:dp,cmd:x", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s6\""), "dead pin, x: %s", o.text);
  act(&o, "role:dp,cmd:y", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s5\""), "dead pin, y: %s", o.text);

  announce(0, "i14~role:dp,cmd:x~1", "role:dp,cmd:x", PORT_BASE + 6, 5555);
  for (int i = 0; i < 3; ++i) {
    act(&o, "role:dp,cmd:x", "{}");
    CHECK(!o.rejected && has(&o, "\"from\":\"s5\""), "x gone, x: %s", o.text);
  }
  {
    json_t data = meta_toJSON("{}");
    mwm_settled_t got = mwm_actE("role:dp,cmd:x", data);

    CHECK(!got.rejected && strcmp(got.value.msg.from, "s5") == 0,
          "x gone, actE: %s", got.value.msg.from.text());
    got.value.release();
    data.release();
  }

  announce(1, "i14~role:dp,cmd:x~2", "role:dp,cmd:x", PORT_BASE + 6, 5555);
  act(&o, "role:dp,cmd:x", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s6\""), "x back: %s", o.text);

  /* ours on the same pin: theirs while it has a target, then ours again */
  announce(1, "i16~role:dq,cmd:x~1", "role:dq,cmd:x", PORT_BASE + 6, 5555);
  act(&o, "role:dq,cmd:x", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s6\""), "dq, theirs: %s", o.text);
  announce(0, "i16~role:dq,cmd:x~1", "role:dq,cmd:x", PORT_BASE + 6, 5555);
  act(&o, "role:dq,cmd:x", "{}");
  CHECK(!o.rejected && has(&o, "\"who\":\"local role:dq,cmd:x\""),
        "dq, ours again: %s", o.text);

  look(&seen, 5, 50);
  look(&seen, 6, 0);
}

static void suite(void) {

  outcome_t o;
  seen_t seen;

  /* role:a - one echo server */
  announce(1, "i1~role:a,cmd:*~1", "role:a,cmd:*", PORT_BASE + 0, 5555);

  act(&o, "role:a,cmd:ping", "{\"n\":1,\"cmd\":\"ping2\"}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s0\"") && has(&o, "\"n\":1"),
        "echo: %s", o.text);
  look(&seen, 0, 50);
  CHECK(seen.accepts == 1 && seen.lines == 1, "echo: %d accepts %d lines",
        seen.accepts, seen.lines);

  /**
   * prepareRequest's line: id (11 hex), k a, sync true, args - the pattern's
   * pairs first, as strings, the args over them in place
   */
  CHECK(strlen(seen.ids[0]) == 11, "id %s", seen.ids[0]);
  CHECK(strstr(seen.last, "\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"a\","
                          "\"cmd\":\"ping2\",\"n\":1}}") != NULL,
        "the line: %s", seen.last);

  /* a second call goes over the same connection */
  act(&o, NULL, "{\"role\":\"a\",\"cmd\":\"x\"}");
  CHECK(!o.rejected && has(&o, "\"cmd\":\"x\""), "args alone: %s", o.text);
  look(&seen, 0, 50);
  CHECK(seen.accepts == 0 && seen.lines == 1, "reused: %d accepts", seen.accepts);

  /* actE: k aE, p the pattern, d the data */
  {
    json_t data = meta_toJSON("{\"x\":1}");
    mwm_settled_t got = mwm_actE("role:a,cmd:e", data);

    CHECK(!got.rejected, "actE rejected");
    look(&seen, 0, 50);
    CHECK(strstr(seen.last, "\"k\":\"aE\",\"sync\":true,\"p\":\"role:a,cmd:e\","
                            "\"d\":{\"x\":1}}") != NULL,
          "actE line: %s", seen.last);
    got.value.release();
    data.release();
  }

  /* no pin matches: undefined, no rejection */
  act(&o, "role:nobody,cmd:x", "{}");
  CHECK(!o.rejected && strcmp(o.text, "undefined") == 0, "no pin: %s", o.text);

  /* role:t - an error the service threw: {err: its fields, msg: the args} */
  announce(1, "i2~role:t,cmd:*~1", "role:t,cmd:*", PORT_BASE + 1, 5555);
  act(&o, "role:t,cmd:x", "{\"q\":2}");
  CHECK(o.rejected && has(&o, "\"err\":{\"message\":\"thrown on purpose\","
                              "\"code\":\"E_PURPOSE\",\"name\":\"Error\"}") &&
            has(&o, "\"msg\":{\"role\":\"t\",\"cmd\":\"x\",\"q\":2}"),
        "thrown: %s", o.text);
  look(&seen, 1, 0);

  /* ... and it is not retried, and opens nothing: the next call goes there */
  act(&o, "role:t,cmd:x", "{}");
  CHECK(o.rejected && has(&o, "thrown on purpose"), "thrown again: %s", o.text);
  look(&seen, 1, 50);
  CHECK(seen.lines == 1, "thrown: %d lines", seen.lines);

  /* role:o - one target, overloaded: no ten tries, no-current-target */
  announce(1, "i3~role:o,cmd:*~1", "role:o,cmd:*", PORT_BASE + 2, 5555);
  act(&o, "role:o,cmd:x", "{}");
  CHECK(o.rejected && has(&o, "\"err\":\"no-current-target\""), "overload: %s",
        o.text);
  CHECK(o.ms >= 95 && o.ms < 400, "overload took %ld ms", o.ms);
  look(&seen, 2, 50);
  CHECK(seen.lines == 1, "overload: %d lines", seen.lines);

  /**
   * The overload of a closing service - message, no details - is not an
   * overload to balance.js: an error for the caller, at once, no retry
   */
  announce(1, "i4~role:c,cmd:*~1", "role:c,cmd:*", PORT_BASE + 3, 5555);
  act(&o, "role:c,cmd:x", "{}");
  CHECK(o.rejected && has(&o, "\"message\":\"retry_later_err_overload\"") &&
            o.ms < 90,
        "closing: %s after %ld ms", o.text, o.ms);
  look(&seen, 3, 50);

  /**
   * role:f - failover: the overloaded one is opened, the retry 100 ms later
   * goes to the other one, with an id of its own
   */
  announce(1, "i5~role:f,cmd:*~1", "role:f,cmd:*", PORT_BASE + 4, 5555);
  announce(1, "i6~role:f,cmd:*~1", "role:f,cmd:*", PORT_BASE + 5, 5555);
  {
    seen_t all[SERVERS];

    act(&o, "role:f,cmd:x", "{}");
    CHECK(!o.rejected && has(&o, "\"from\":\"s5\"") && o.ms >= 95,
          "failover: %s after %ld ms", o.text, o.ms);
    lookAll(all, 50);
    CHECK(all[4].lines == 1 && all[5].lines == 1 &&
              strcmp(all[4].ids[0], all[5].ids[0]) != 0,
          "failover: %d and %d lines, ids %s and %s", all[4].lines,
          all[5].lines, all[4].ids[0], all[5].ids[0]);

    /* s4 is open: straight to s5, no wait */
    act(&o, "role:f,cmd:x", "{}");
    CHECK(!o.rejected && has(&o, "\"from\":\"s5\"") && o.ms < 90,
          "while open: %s after %ld ms", o.text, o.ms);
    lookAll(all, 0);
    CHECK(all[4].lines == 0 && all[5].lines == 1, "while open: %d and %d lines",
          all[4].lines, all[5].lines);

    /**
     * Round robin with one shared cursor: the cursor is past s4 now, so it
     * takes s4 again only after closingTimeout, when it is half-open - and
     * then it is overloaded again and the call goes on to s5
     */
    meta_sleepMs(1000);
    act(&o, "role:f,cmd:x", "{}");
    CHECK(!o.rejected && has(&o, "\"from\":\"s5\""), "half-open: %s",
          o.text);
    lookAll(all, 0);
  }

  /**
   * role:d - a target nobody listens on: refused, which is a timeout once
   * connectTimeout (355) has passed, then the retry goes to the live one
   */
  announce(1, "i7~role:d,cmd:*~1", "role:d,cmd:*", NOBODY_PORT, 5555);
  announce(1, "i8~role:d,cmd:*~1", "role:d,cmd:*", PORT_BASE + 6, 5555);
  act(&o, "role:d,cmd:x", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s6\"") && o.ms >= 440 && o.ms < 900,
        "dead target: %s after %ld ms", o.text, o.ms);
  look(&seen, 6, 0);

  /* role:m - no answer within the target's timeout (announced as 300) */
  announce(1, "i9~role:m,cmd:*~1", "role:m,cmd:*", PORT_BASE + 7, 300);
  act(&o, "role:m,cmd:x", "{}");
  CHECK(o.rejected && has(&o, "no-current-target") && o.ms >= 390 && o.ms < 800,
        "timeout: %s after %ld ms", o.text, o.ms);
  look(&seen, 7, 0);

  /**
   * role:j - every kind of line that is not an answer comes first: each is
   * dropped, the answer is taken, and the connection is still the same one
   */
  announce(1, "i10~role:j,cmd:*~1", "role:j,cmd:*", PORT_BASE + 8, 2000);
  act(&o, "role:j,cmd:x", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s8\""), "junk: %s after %ld ms",
        o.text, o.ms);
  act(&o, "role:j,cmd:y", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s8\"") && o.ms < 200,
        "junk, second call: %s after %ld ms", o.text, o.ms);
  look(&seen, 8, 50);
  CHECK(seen.accepts == 1 && seen.ends == 0, "junk: %d accepts, %d ends",
        seen.accepts, seen.ends);

  /**
   * its last member removed, the pin leaves the tree (fix/dead-pin): no pin
   * matches any more - undefined, as from a pin that never was
   */
  announce(0, "i1~role:a,cmd:*~1", "role:a,cmd:*", PORT_BASE + 0, 5555);
  act(&o, "role:a,cmd:ping", "{}");
  CHECK(!o.rejected && strcmp(o.text, "undefined") == 0 && o.ms < 50,
        "removed: %s", o.text);
  look(&seen, 0, 100);
  CHECK(seen.ends == 1, "removed: the connection was not closed (%d ends)",
        seen.ends);

  /* the same member announced twice is one target */
  announce(1, "i11~role:k,cmd:*~1", "role:k,cmd:*", PORT_BASE + 9, 5555);
  announce(1, "i11~role:k,cmd:*~1", "role:k,cmd:*", PORT_BASE + 9, 5555);
  act(&o, "role:k,cmd:x", "{}");
  CHECK(!o.rejected, "keepalive target: %s", o.text);
  look(&seen, 9, 0);

  /**
   * Our handlers and the mesh's pins in one match, as in index.js: our
   * role:lw,cmd:ping beats role:lw,cmd:* of s6, role:lx,cmd:* of s6 beats
   * our role:lx - more pairs first, then more exact ones
   */
  announce(1, "i12~role:lw,cmd:*~1", "role:lw,cmd:*", PORT_BASE + 6, 5555);
  announce(1, "i13~role:lx,cmd:*~1", "role:lx,cmd:*", PORT_BASE + 6, 5555);

  act(&o, "role:lw,cmd:ping", "{}");
  CHECK(!o.rejected && has(&o, "\"who\":\"local role:lw,cmd:ping\""),
        "ours, exact: %s", o.text);
  act(&o, "role:lw,cmd:pong", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s6\""), "theirs, wildcard: %s",
        o.text);
  act(&o, "role:lx,cmd:go", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s6\""),
        "theirs, more pairs: %s", o.text);
  act(&o, NULL, "{\"role\":\"lx\"}");
  CHECK(!o.rejected && has(&o, "\"who\":\"local role:lx\""),
        "ours, only one that takes it: %s", o.text);
  {
    json_t data = meta_toJSON("{}");
    mwm_settled_t got = mwm_actE("cmd:ping,role:lw", data);

    CHECK(!got.rejected &&
              strcmp(got.value.who, "local role:lw,cmd:ping") == 0,
          "actE, ours: %s", got.value.who.text());
    got.value.release();
    data.release();
  }
  look(&seen, 6, 50);
  CHECK(seen.lines == 2, "s6 got %d calls, not the two that are its", seen.lines);

  deadPin();
}

/**
 * The keepalive, on short clocks: {"h":1} every 300 ms, 200 ms for an answer.
 * A server that answers keeps its connection; one that does not loses it,
 * and the next call makes a new one.
 */
static void keepaliveSuite(void) {

  outcome_t o;
  seen_t seen;

  /* role:k (server 9, ECHO) answers {"h":1} */
  act(&o, "role:k,cmd:x", "{}");
  look(&seen, 9, 900);
  CHECK(seen.heartbeats >= 2 && seen.ends == 0,
        "keepalive answered: %d heartbeats, %d ends", seen.heartbeats, seen.ends);

  /**
   * role:h (server 10, DEAF) does not: dropped at the second look - the
   * answer to the call is traffic for the first, as keepalive.called is set
   * by any line and reset only at the look
   */
  announce(1, "i12~role:h,cmd:*~1", "role:h,cmd:*", PORT_BASE + 10, 5555);
  act(&o, "role:h,cmd:x", "{}");
  CHECK(!o.rejected, "deaf: %s", o.text);
  look(&seen, 10, 1200);
  CHECK(seen.heartbeats >= 1 && seen.ends == 1,
        "keepalive unanswered: %d heartbeats, %d ends", seen.heartbeats,
        seen.ends);

  act(&o, "role:h,cmd:y", "{}");
  CHECK(!o.rejected, "deaf again: %s", o.text);
  look(&seen, 10, 0);
  CHECK(seen.accepts == 1, "deaf: reconnected %d times", seen.accepts);
}

/**
 * patient, the default: one target overloaded once. 1.6.2 gives up with
 * no-current-target after 100 ms; patient waits once for it to be half-open
 * and gets the answer about 1.2 s in.
 */
static void patientSuite(void) {

  outcome_t o;
  seen_t seen;

  announce(1, "p1~role:p,cmd:*~1", "role:p,cmd:*", PORT_BASE + 11, 5555);
  act(&o, "role:p,cmd:x", "{}");
  CHECK(!o.rejected && has(&o, "\"from\":\"s11\"") && o.ms >= 1150 &&
            o.ms < 1700,
        "patient: %s after %ld ms", o.text, o.ms);
  look(&seen, 11, 0);
  CHECK(seen.lines == 2, "patient: %d lines", seen.lines);

  /* always overloaded: one wait, then no-current-target */
  announce(1, "p2~role:q,cmd:*~1", "role:q,cmd:*", PORT_BASE + 2 + 10, 5555);
  act(&o, "role:q,cmd:x", "{}");
  CHECK(o.rejected && has(&o, "no-current-target") && o.ms >= 1250 &&
            o.ms < 1800,
        "patient, always overloaded: %s after %ld ms", o.text, o.ms);
  look(&seen, 12, 0);
  CHECK(seen.lines == 2, "patient, always: %d lines", seen.lines);

  /* and with a timeout the budget is spent after one try: no retry */
  announce(1, "p3~role:r,cmd:*~1", "role:r,cmd:*", PORT_BASE + 13, 5555);
  act(&o, "role:r,cmd:x", "{}");
  CHECK(o.rejected && has(&o, "all-targets-overloaded") && o.ms >= 5500 &&
            o.ms < 5900,
        "patient, timeout: %s after %ld ms", o.text, o.ms);
}

/**
 * --short: transport timeout 400 ms, closingTimeout 100, retryTimeout 10 -
 * two targets that read and never answer. Each is half-open again by its
 * next turn, which in 1.6.2 made it ten full timeouts; patient, the call is
 * over within the one request timeout.
 */
static void shortSuite(void) {

  outcome_t o;

  announce(1, "m1~role:m,cmd:*~1", "role:m,cmd:*", PORT_BASE + 7, 400);
  announce(1, "m2~role:m,cmd:*~1", "role:m,cmd:*", PORT_BASE + 13, 400);
  act(&o, "role:m,cmd:x", "{\"n\":1}");
  CHECK(o.rejected && has(&o, "all-targets-overloaded") && o.ms >= 350 &&
            o.ms < 600,
        "short, both silent: %s after %ld ms", o.text, o.ms);
}

typedef enum { RUN_EXACT, RUN_PATIENT, RUN_SHORT } run_t;

/** One task for all of it: the checks are counted in one place. */
static void everything(run_t run) {

  if (run == RUN_PATIENT) {
    patientSuite();
    return;
  }

  if (run == RUN_SHORT) {
    shortSuite();
    return;
  }

  suite();
  keepaliveSuite();
}

int main(int argc, char **argv) {

  mwm_client_options_t options;
  static const mode_t_ modes[] = {ECHO, THROW, OVER,      CLOSING, OVER,
                                  ECHO, ECHO,  MUTE,      JUNK,    ECHO,
                                  DEAF, ONCE_OVER, OVER,  MUTE};
  enum { COUNT = sizeof modes / sizeof modes[0] };
  tcpListener_t *listeners[COUNT];
  run_t run = argc > 2 && strcmp(argv[2], "--no-patient") == 0 ? RUN_EXACT
              : argc > 2 && strcmp(argv[2], "--short") == 0    ? RUN_SHORT
                                                               : RUN_PATIENT;
  static const char *const labels[] = {"", " (patient)", " (patient, 400 ms)"};

  for (int i = 0; i < COUNT; ++i) {

    listeners[i] = meta_tcpListen("127.0.0.1", PORT_BASE + i);

    if (listeners[i].failed()) {
      fprintf(stderr, "client: cannot listen on %d: %s\n", PORT_BASE + i,
              listeners[i].error());
      listeners[i].close();
      return 1;
    }

    go servers fakeServe(listeners[i], i, modes[i]);
  }

  mwm_clientDefaults(&options);
  snprintf(options.instance, sizeof options.instance, "ffffffffff0");
  options.keepaliveMs = 300;
  options.keepaliveWaitMs = 200;
  if (run == RUN_EXACT)
    options.balance.patient = 0;

  if (run == RUN_SHORT) {
    options.timeout = 400;
    options.balance.closingTimeout = 100;
    options.balance.retryTimeout = 10;
  }

  /* handlers of our own, before the client: they compete with the mesh's */
  mwm_add("role:lw,cmd:ping", localPing);
  mwm_add("role:lx", localX);
  mwm_add("role:dq,cmd:x", localQ);

  mwm_clientStart(&options);

  task {
    go everything(run);
  }

  mwm_clientStop();

  for (int i = 0; i < COUNT; ++i)
    listeners[i].stop();

  join servers;

  for (int i = 0; i < COUNT; ++i)
    listeners[i].close();

  notes.close();

  for (n in notes)
    free(n.text);

  mwm_clear();

  if (failures != 0) {
    fprintf(stderr, "client%s: %d of %d checks failed\n", labels[run],
            failures, checked);
    return 1;
  }

  printf("client%s: %d checks ok\n", labels[run], checked);
  return 0;
}
