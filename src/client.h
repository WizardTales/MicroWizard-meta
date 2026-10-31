/**
 * The calling half of microwizard: act and actE from meta into the mesh,
 * the way a Node service calls - pin by pin, target by target, with
 * balance.js and visigoth deciding who gets a call.
 *
 * Plain C, read by meta and by the C compiler; client.c is the how and
 * docs/protocol.md sections 9 to 11 the what. In short:
 *
 *   - the mesh tells the client about members (mwm_clientMember, from the
 *     sneeze table in mesh.c). Every pin a member announces is a target,
 *     keyed as mesh.js keys it: utilPattern(pinConfig) + '~' + identifier$.
 *     Targets of one pin form a group with a visigoth in front, as
 *     balance.js has one per pin. A member of our own instance, a pin we
 *     serve ourselves, null:true and anything not tcp are not targets.
 *   - every target has one long-lived ndjson connection, dialed on the
 *     first call with the target's connectTimeout (355 ms unless it says
 *     otherwise), kept alive with {"h":1} every two minutes and dropped when
 *     nothing at all came back within five seconds of it. A call waits
 *     timeout (5555) for its answer.
 *   - a call is consume from balance.js (balance.h): visigoth chooses, a
 *     timeout or an overload reply opens the target and the call is tried
 *     again 100 ms later with a new id, up to ten times; patient (the
 *     default) waits once for an opened target to be half-open and keeps
 *     the whole call within one request timeout.
 *   - an answer that cannot be used - not JSON, not an object, another k, a
 *     null or unknown id - is dropped and the connection kept, as
 *     microwizard does since fix/unknown-reply.
 *
 * Nothing is shared and nothing is locked. What changes with the mesh -
 * targets, visigoths, the pin tree - is one value passed from task to task
 * over a channel of capacity one: a caller holds it for the match and the
 * choice, the mesh for a member. A connection belongs to its reader task,
 * which owns the table of ids waiting on it. The caller writes its own
 * request line and the reader hands it the answer: one handoff per call,
 * and a call runs in the caller's own task; there is no task per request.
 *
 * The fence of main.c holds here too: unless `anyAddress` is set, the
 * client dials 127.0.0.0/8 only, and never a name - a host that is not an
 * address literal is not a target.
 */
#ifndef MWM_CLIENT_H
#define MWM_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include <meta_json.h>
#include <meta_owns.h>

#include "balance.h"

typedef struct {
  /**
   * balance.js's numbers, and the patient option (on). The budget of a
   * patient call is 0 here, which means `timeout` below: one request
   * timeout, as Micro hands the transport's to its balancer.
   */
  bal_options_t balance;

  /** tcp.js: {"h":1} this often, then this long for any line back. */
  long keepaliveMs;     /* 120000 */
  long keepaliveWaitMs; /* 5000 */

  /** transfer.js's own settings, for a member whose config lacks them. */
  long timeout;         /* 5555 */
  long connectTimeout;  /* 355 */

  /** How often the client looks at its clocks. */
  long tickMs;          /* 100 */

  /** Lift the loopback fence. */
  int anyAddress;

  /** One JSON line on stdout per target added or removed. */
  int events;

  /** Our instance: members of it are not targets (mesh.js "ignore myself"). */
  char instance[32];

  /** The pins we serve, as written: not targets (mesh.js ignoreClient). */
  const char *const *ownPins;
  int ownPinCount;
} mwm_client_options_t;

void mwm_clientDefaults(mwm_client_options_t *into);

/** Starts the client's tasks. Once per process. */
int mwm_clientStart(const mwm_client_options_t *options);

/**
 * Ends them: every call still waiting is answered as timed out, every
 * connection closed. Returns once all of the client's tasks are gone.
 */
void mwm_clientStop(void);

/**
 * A member added or removed by the mesh (mesh.c's emit), with its meta as
 * msgpack. Copied; safe from any task.
 */
void mwm_clientMember(int added, const char *identifier, const uint8_t *meta,
                      size_t metaLen);

/**
 * What actAsync settles with: the value, owned by the caller, and whether it
 * is a rejection.
 *
 *   rejected 0   the answer's res - a document that isNothing when the
 *                answer had none, or when no pin matched at all (microwizard
 *                resolves undefined then and logs no-target)
 *   rejected 1   {err, msg}: err the error the service threw (its fields,
 *                message and name), or the text 'no-current-target' or
 *                'all-targets-overloaded'; msg the args that were sent
 */
typedef struct {
  int rejected;
  json_t value;
} mwm_settled_t;

/**
 * act: a pattern such as "role:cdn,cmd:purge" (or NULL) with args over it,
 * as `mc.act(pattern, args)` - or args alone, as `mc.act(args)`.
 *
 * Our handlers (mwm_add, before mwm_clientStart) and the pins of the mesh
 * are matched as one, as in Node (pattern.h): when a handler of ours wins,
 * it answers here, without the mesh - its result, or rejected 1 with what it
 * threw, as a local act in Node. A pin whose last target is gone is out of
 * the match until a member announces it again (microwizard fix/dead-pin).
 *
 * Parks the calling task, never a carrier.
 */
mwm_settled_t mwm_act(const char *pattern, json_t args);

/**
 * actE: matched on the pattern alone, sent as k "aE" with p the pattern and
 * d the data; the service hands its handler d. The rest as mwm_act.
 */
mwm_settled_t mwm_actE(const char *pattern, json_t data);

#endif
