/**
 * The serving half of microwizard's transport, without the socket: what a
 * line that came in over TCP asks for, which handler answers it, and the line
 * that goes back. main.c owns the connections and calls mwm_respond per line.
 *
 * Plain C, read by meta and by the C compiler; service.c is the how and
 * docs/protokoll.md section 10 the what. In short:
 *
 *   {"h":1}                                  -> {"h":2}
 *   {"id","k":"a","sync":true,"args":{...}}  -> {"id","k":"res","sync":true,"res":...}
 *   {"id","k":"aE","sync":true,"p":"pin","d":{...}}   the same, matched on p
 *   a handler that throws                    -> {"id","k":"res","sync":true,"error":{...}}
 *   while going offline                      -> error retry_later_err_overload
 *   too many calls running (mwm_refuse)      -> the same, with details, which
 *                                               a caller's balancer retries
 *   sync false, an unknown k                 -> nothing at all
 *
 * A handler is matched the way microwizard matches since fix/pattern-shadow,
 * by pattern.c: every primitive field on the top level of args is a
 * `key:value` (null, objects and arrays are not), for aE the pairs of p; a
 * pattern matches when all of its pairs are among them - `*` for any value -
 * and of those the one with the most pairs wins, then the one with the most
 * exact pairs, then the one exact on the first key that tells them apart.
 * Only our own handlers answer here: a call that came in is not passed on.
 */
#ifndef MWM_SERVICE_H
#define MWM_SERVICE_H

#include <stddef.h>

#include <meta_json.h>
#include <meta_owns.h>

#include "pattern.h"

/**
 * What a handler answers: a document, owned by the caller, and whether it
 * was thrown.
 *
 *   thrown 0   the document is the result, `res` on the wire - the house
 *              convention is {code, msg} or {code >= 400, err}
 *   thrown 1   the document is the thrown error's fields: message, and
 *              whatever else (code, details ...); name defaults to "Error"
 *
 * A document that answers isNothing is `res: undefined`: an answer without
 * res. A handler builds the document and returns both at once:
 *
 *   json_t error = {message: "no such zone", code: "E_ZONE"};
 *   return (mwm_answer_t){1, error};
 */
typedef struct {
  int thrown;
  json_t value;
} mwm_answer_t;

/** A handler: the args (or for aE the d) in, the answer out. */
typedef mwm_answer_t (*mwm_handler_t)(json_t args);

/**
 * Adds a handler for a pattern such as "role:cdn,cmd:purge" - before serving
 * and before mwm_clientStart, whose matching takes them in then. A pattern
 * with the same pairs as one before takes its place. 0 for a pattern without
 * a pair.
 */
int mwm_add(const char *pattern, mwm_handler_t handler);

/**
 * mwm_add for a handler that never parks - no I/O, no sleep, no act, no
 * channel, nothing that waits on anything: the connection's reader runs it
 * itself, as Node's event loop runs a handler, instead of starting a task
 * for it. Whether a function parks is not something meta can tell from
 * here, so the one adding it says so. A handler that may park belongs in
 * mwm_add: run by the reader, it would hold up every call behind it on its
 * connection.
 */
int mwm_addInline(const char *pattern, mwm_handler_t handler);

/** Drops every handler. */
void mwm_clear(void);

/** From now on every request is answered with retry_later_err_overload. */
void mwm_goOffline(void);
int mwm_isOffline(void);

/**
 * The line to send back for one line that came in, without its newline - or
 * NULL when nothing goes back. The caller frees it.
 */
META_OWNS char *mwm_respond(const char *line, size_t length);

/** What became of one line, read once by mwm_respondInline. */
typedef enum {
  MWM_BROKEN,   /* not JSON: the connection ends, as tcp.js's parser error does */
  MWM_ANSWERED, /* answered here: the line in `out`, or nothing to send */
  MWM_LATER     /* a handler that may park: mwm_respond, in a task of its own */
} mwm_reading_t;

typedef struct {
  mwm_reading_t reading;
  char *out; /* MWM_ANSWERED: the line to send back, or NULL; the caller frees it */
} mwm_inline_t;

/**
 * One line, parsed once, answered where it can be without a task: a health
 * ping, a line that is no call, every call while offline, a call no handler
 * matches and a call of a handler added with mwm_addInline. Such a call runs
 * under the gate as any other - counted in `running` while it runs, refused
 * as overloaded when that would make more than `max`.
 */
mwm_inline_t mwm_respondInline(const char *line, size_t length, long *running,
                               long max);

/** The handlers in the order added, for client.c's tree. */
int mwm_handlerCount(void);
const char *mwm_handlerPattern(int i);
mwm_handler_t mwm_handlerAt(int i);

/**
 * The answer to a call refused because too many are running - error
 * retry_later_err_overload with details.message saying the same, which a
 * caller's balancer counts as overload - or {"h":2} for a health ping, or
 * NULL when nothing goes back. The caller frees it.
 */
META_OWNS char *mwm_refuse(const char *line, size_t length);

/** Our handler for a message (pattern.h), or NULL. Sorts the parts. */
mwm_handler_t mwm_findHandler(mwm_parts_t *parts);

#endif
