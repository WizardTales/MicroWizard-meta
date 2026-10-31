/**
 * The target choice and circuit breaker of @wzrdtales/visigoth 2.0.5, which
 * microwizard's balance.js puts in front of every pin.
 *
 * Ported decision for decision, quirks included, because a meta client in
 * the mesh should spread load and back off exactly as a Node one does.
 * docs/protokoll.md ("visigoth") has the behaviour in prose; the short form:
 *
 *   - a ring of targets in insertion order (scores are all 1 in microwizard),
 *     one shared cursor that only moves forward: round robin
 *   - CLOSED and HALF-OPEN targets are chosen; OPEN ones are stepped over
 *   - one reported failure opens a target, there is no failure count
 *   - an OPEN target is HALF-OPEN once more than closingTimeout ms passed
 *     since it opened, and it is CLOSED again the moment it is *chosen* -
 *     not when the call succeeds; a failure of that call opens it again
 *   - when every target is OPEN the choice fails ("no upstreams available")
 *
 * Time is passed in, in milliseconds, so a test can replay a sequence.
 */
#ifndef MWM_VISIGOTH_H
#define MWM_VISIGOTH_H

#include <stddef.h>

typedef enum { VG_CLOSED = 0, VG_OPEN, VG_HALF_OPEN } vg_status_t;

typedef struct vg_node_t {
  struct vg_node_t *next;
  struct vg_node_t *before;
  int score;

  vg_status_t status;
  long statusTimestamp;
  long lastChosen;

  /** The caller's; visigoth never looks at it. */
  void *target;
} vg_node_t;

typedef struct {
  vg_node_t *top;
  vg_node_t *it;
  size_t length;

  /** BalanceLinkedRing's size: 40 unless said otherwise. */
  size_t size;

  /** 30000 in visigoth itself; microwizard's balance.js passes 1000. */
  long closingTimeout;
} vg_t;

#define VG_RING_SIZE 40
#define VG_CLOSING_TIMEOUT 30000
#define VG_MICROWIZARD_CLOSING_TIMEOUT 1000

void vg_t__init(vg_t *self, long closingTimeout, size_t ringSize);

/** Gives every node back. */
void vg_t__release(vg_t *self);

/** A new CLOSED target, or NULL when the ring is full (visigoth: "ring full"). */
vg_node_t *vg_t__add(vg_t *self, void *target, int score, long now);

/** Takes a node out of the ring and frees it. */
void vg_t__remove(vg_t *self, vg_node_t *node);

/** The node chosen, or NULL for "no upstreams available". */
vg_node_t *vg_t__choose(vg_t *self, long now);

/** What visigoth's failure handler does: the target is OPEN from now. */
void vg_node_t__failed(vg_node_t *self, long now);

const char *vg_statusName(vg_status_t status);

#endif
