/**
 * The member table sneeze keeps on top of swim: who is in the mesh, by
 * identifier$, and when a member is added or removed.
 *
 * Plain C, read by meta and by the C compiler; mesh.c is the how. swim keys
 * members by the "ip:port" they announce; sneeze keys them by the
 * identifier$ in their meta, and that is what a consumer sees - microwizard
 * makes a client per pin out of every `add` and drops it on `remove`.
 *
 * The rules are sneeze-ng 0.13.1 (fix/restart-same-host), which differ from
 * 0.13.0 in two places that matter when a node comes back fast:
 *
 *   - a member at a host we already hold under another identifier$, with an
 *     incarnation at least as new, *replaces* it: the old one is removed
 *     first, then the new one added. swim only sees the host updated, so
 *     without this the old identifier never goes faulty and stays a target.
 *   - news about an older incarnation loses: a member held at another host
 *     with a newer incarnation is not replaced by it, nor is one held at the
 *     same host under another identifier$ with a newer incarnation; and a
 *     faulty for an identifier$ at a host it is no longer held at removes
 *     nothing.
 *
 * Members without meta, with another tag$ or with our own identifier$ are
 * passed over, as sneeze passes them. Alive adds - again only when the
 * incarnation, host or v$ moved - and faulty removes; suspect says nothing.
 *
 * One table per listen, as there is one sneeze per listen in microwizard: a
 * process with two listens has each listen see the other as a member. That
 * the other is ours (same instance) is for the layer above to know - mesh.js
 * passes over members of its own instance, and so does main.c.
 */
#ifndef MWM_MESH_H
#define MWM_MESH_H

#include <stddef.h>
#include <stdint.h>

#include "swim.h"

#define MESH_ID_MAX 512

typedef struct {
  char identifier[MESH_ID_MAX];
  char host[SWIM_HOST_MAX];
  int64_t incarnation;
  long version;

  /** The member's meta as msgpack, as it came - for whoever is told. */
  uint8_t *meta;
  size_t metaLen;
} mesh_peer_t;

/** Told for every add and remove; `peer` is only good for the call. */
typedef void (*mesh_emit_t)(void *context, int added, const mesh_peer_t *peer);

typedef struct {
  /** "mc~<tag>": the only members we report. */
  char tagged[80];

  /** Our own identifier$ - this listen's. */
  char identifier[MESH_ID_MAX];

  /** Who is held, in the order they came; mesh.c's. */
  struct mesh_peers *peers;

  mesh_emit_t emit;
  void *context;
} mesh_t;

void mesh_init(mesh_t *self, const char *tagged, const char *identifier,
               mesh_emit_t emit, void *context);

void mesh_free(mesh_t *self);

/** What swim reports (Change or Update) about a member that is not us. */
void mesh_update(mesh_t *self, const swim_member_t *member);

/** The member held under an identifier$, or NULL. */
const mesh_peer_t *mesh_find(const mesh_t *self, const char *identifier);

#endif
