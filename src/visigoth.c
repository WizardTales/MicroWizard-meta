/**
 * visigoth.js and balanceLinkedRing.js, line for line where it matters.
 * visigoth.h says what it does; the comments here say where the JS is odd.
 */
#include <stdlib.h>

#include "visigoth.h"

void vg_t.init(vg_t *self, long closingTimeout, size_t ringSize) {

  self->top = NULL;
  self->it = NULL;
  self->length = 0;
  self->size = ringSize != 0 ? ringSize : VG_RING_SIZE;

  /* `closingTimeout || 30000`: a zero means the default there too */
  self->closingTimeout = closingTimeout != 0 ? closingTimeout : VG_CLOSING_TIMEOUT;
}

void vg_t.release(vg_t *self) {

  vg_node_t *at = self->top;

  while (at != NULL) {

    vg_node_t *next = at->next;

    free(at);
    at = next;
  }

  self->top = NULL;
  self->it = NULL;
  self->length = 0;
}

/**
 * BalanceLinkedRing.addScore. Two things in it never matter in microwizard,
 * where every score is 1, and are kept anyway:
 *
 *   - a score above the top's is linked in front of it but never made the
 *     new top, so walking from the top never reaches it
 *   - the walk for a lower score stops one element late, after the first
 *     lower-scored one rather than before it
 *
 * With equal scores a new target goes to the end of the ring.
 */
vg_node_t *vg_t.add(vg_t *self, void *target, int score, long now) {

  vg_node_t *e;

  if (self->length != 0 && self->length >= self->size)
    return NULL;

  e = (vg_node_t *)calloc(1, sizeof *e);

  if (e == NULL)
    return NULL;

  e->score = score;
  e->status = VG_CLOSED;
  e->statusTimestamp = now;
  e->lastChosen = -1;
  e->target = target;

  if (self->length == 0) {
    self->length = 1;
    self->top = e;
    self->it = e;
    return e;
  }

  self->length += 1;

  if (score > self->top->score) {
    e->next = self->top;
    self->top->before = e;
  } else {

    vg_node_t *c = self->top;

    while (c->next != NULL && (c = c->next)->score >= score)
      ;

    if (c->next != NULL) {
      e->next = c->next;
      c->next->before = e;
    } else
      e->before = c;

    c->next = e;
  }

  return e;
}

/** getAndStep: hand out the cursor, move it on, wrap to the top. */
static vg_node_t *getAndStep(vg_t *self) {

  vg_node_t *r = self->it;

  self->it = self->it->next != NULL ? self->it->next : self->top;

  return r;
}

/**
 * removeElement. The length goes down before anything is checked, so taking
 * out a node twice would count it twice; balance.js keeps its own map and
 * never does.
 */
void vg_t.remove(vg_t *self, vg_node_t *e) {

  if (e == NULL)
    return;

  self->length -= 1;

  if (self->length == 0) {
    self->it = NULL;
    self->top = NULL;
  }

  if (self->it == e)
    self->it = e->next != NULL ? e->next : self->top;

  if (e->next != NULL) {
    if (e->before != NULL)
      e->next->before = e->before;
    else {
      e->next->before = NULL;
      self->top = e->next;
    }
  }

  if (e->before != NULL)
    e->before->next = e->next;

  free(e);
}

/**
 * visigoth's choose with its default rater, which answers 1 for everyone - so
 * the rater's "a score of zero opens the target" never fires and is left out.
 *
 * The loop starts at the cursor and goes round once at most. An OPEN target
 * whose closingTimeout has passed (strictly more than) turns HALF-OPEN on the
 * spot and is taken; one that has not is stepped over. Every step moves the
 * shared cursor, so after a choice past open targets the next choice starts
 * behind it.
 */
vg_node_t *vg_t.choose(vg_t *self, long now) {

  vg_node_t *start;
  vg_node_t *upstream;
  int found = 0;

  if (self->length == 0)
    return NULL;

  start = getAndStep(self);
  upstream = start;

  do {
    if (upstream->status == VG_OPEN) {
      if (now - upstream->statusTimestamp > self->closingTimeout) {
        upstream->status = VG_HALF_OPEN;
        upstream->statusTimestamp = now;
      } else {
        upstream = getAndStep(self);
        continue;
      }
    }

    if (upstream->status != VG_OPEN) {
      found = 1;
      break;
    }

    upstream = getAndStep(self);
  } while (upstream != start);

  if (!found)
    return NULL;

  upstream->lastChosen = now;

  /**
   * "Close the circuit once it has been successful", says the JS - but this
   * runs right after the callback that *starts* the call, so a half-open
   * target is closed on being chosen, and every request after it goes there
   * too until one of them reports a failure.
   */
  if (upstream->status == VG_HALF_OPEN) {
    upstream->status = VG_CLOSED;
    upstream->statusTimestamp = now;
  }

  return upstream;
}

void vg_node_t.failed(vg_node_t *self, long now) {

  self->status = VG_OPEN;
  self->statusTimestamp = now;
}

const char *vg_statusName(vg_status_t status) {

  switch (status) {
  case VG_CLOSED:
    return "CLOSED";
  case VG_OPEN:
    return "OPEN";
  default:
    return "HALF-OPEN";
  }
}
