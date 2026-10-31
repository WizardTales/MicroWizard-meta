/**
 * sneeze-ng's add_node and remove_node, with the rules of 0.13.1 for a node
 * that comes back on the same host. mesh.h says what; the comments here say
 * where sneeze is particular.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mesh.h"
#include "msgpack.h"

struct mesh_peers {
  mesh_peer_t *[] all;
};

static void copyText(char *into, size_t room, const char *from) {
  snprintf(into, room, "%s", from != NULL ? from : "");
}

void mesh_init(mesh_t *self, const char *tagged, const char *identifier,
               mesh_emit_t emit, void *context) {

  memset(self, 0, sizeof *self);
  copyText(self->tagged, sizeof self->tagged, tagged);
  copyText(self->identifier, sizeof self->identifier, identifier);
  self->emit = emit;
  self->context = context;
}

static void peerFree(mesh_peer_t *peer) {

  if (peer == NULL)
    return;

  free(peer->meta);
  free(peer);
}

void mesh_free(mesh_t *self) {

  if (self->peers == NULL)
    return;

  for (p in self->peers->all)
    peerFree(*p);

  self->peers->all.release();
  free(self->peers);
  self->peers = NULL;
}

static size_t countOf(const mesh_t *self) {
  return self->peers != NULL ? self->peers->all.count : 0;
}

static mesh_peer_t *peerAt(const mesh_t *self, size_t at) {
  return self->peers->all[at];
}

/** Where the peer under an identifier$ sits in the table, or NULL. */
static mesh_peer_t **placeOf(const mesh_t *self, const char *identifier) {

  if (self->peers == NULL)
    return NULL;

  return self->peers->all.find(p, strcmp((*p)->identifier, identifier) == 0);
}

static long indexOf(const mesh_t *self, const char *identifier) {

  mesh_peer_t **at = placeOf(self, identifier);

  return at != NULL ? (long)(at - self->peers->all.items) : -1;
}

const mesh_peer_t *mesh_find(const mesh_t *self, const char *identifier) {

  mesh_peer_t **at = placeOf(self, identifier);

  return at != NULL ? *at : NULL;
}

/** Takes the peer at `at` out of the table, tells, and frees it. */
static void dropAt(mesh_t *self, size_t at) {

  mesh_peer_t *peer = peerAt(self, at);
  size_t count = countOf(self);

  memmove(&self->peers->all.items[at], &self->peers->all.items[at + 1],
          (count - at - 1) * sizeof peer);
  self->peers->all.count -= 1;

  self->emit(self->context, 0, peer);
  peerFree(peer);
}

static int setMeta(mesh_peer_t *peer, const swim_member_t *m) {

  uint8_t *copy = (uint8_t *)malloc(m->metaLen > 0 ? m->metaLen : 1);

  if (copy == NULL)
    return 0;

  memcpy(copy, m->meta, m->metaLen);
  free(peer->meta);
  peer->meta = copy;
  peer->metaLen = m->metaLen;

  return 1;
}

/**
 * add_node. The order of the checks is sneeze's: the same news again is
 * nothing; news from an older join attempt of a member we hold elsewhere is
 * nothing; then whoever else is held at this host goes - unless one of them
 * is newer, in which case this is the stale one and nothing happens.
 */
static void addNode(mesh_t *self, const swim_member_t *m,
                    const char *identifier, long version) {

  long at = indexOf(self, identifier);
  mesh_peer_t *peer;

  if (at >= 0) {

    mesh_peer_t *prev = peerAt(self, (size_t)at);

    if (prev->incarnation == m->incarnation &&
        strcmp(prev->host, m->host) == 0 && prev->version == version)
      return;

    if (strcmp(prev->host, m->host) != 0 && prev->incarnation > m->incarnation)
      return;
  }

  for (size_t i = 0; i < countOf(self); ++i) {

    mesh_peer_t *other = peerAt(self, i);

    if (strcmp(other->host, m->host) == 0 &&
        strcmp(other->identifier, identifier) != 0 &&
        other->incarnation > m->incarnation)
      return;
  }

  for (size_t i = 0; i < countOf(self);) {

    mesh_peer_t *other = peerAt(self, i);

    if (strcmp(other->host, m->host) == 0 &&
        strcmp(other->identifier, identifier) != 0)
      dropAt(self, i);
    else
      ++i;
  }

  /* dropping may have moved it */
  at = indexOf(self, identifier);

  if (at >= 0)
    peer = peerAt(self, (size_t)at);
  else {

    if (self->peers == NULL &&
        (self->peers = (struct mesh_peers *)calloc(1, sizeof *self->peers)) ==
            NULL)
      return;

    peer = (mesh_peer_t *)calloc(1, sizeof *peer);

    if (peer == NULL)
      return;

    copyText(peer->identifier, sizeof peer->identifier, identifier);

    if (!self->peers->all.push(peer)) {
      free(peer);
      return;
    }
  }

  if (!setMeta(peer, m))
    return;

  copyText(peer->host, sizeof peer->host, m->host);
  peer->incarnation = m->incarnation;
  peer->version = version;

  self->emit(self->context, 1, peer);
}

/** remove_node: an abandoned join attempt going faulty is not the node leaving. */
static void removeNode(mesh_t *self, const swim_member_t *m,
                       const char *identifier) {

  long at = indexOf(self, identifier);

  if (at < 0 || strcmp(peerAt(self, (size_t)at)->host, m->host) != 0)
    return;

  dropAt(self, (size_t)at);
}

void mesh_update(mesh_t *self, const swim_member_t *m) {

  json_t meta;
  const char *identifier;

  if (m->meta == NULL)
    return;

  meta = mp_decode(m->meta, m->metaLen);
  defer meta.release();

  if (meta._refused != NULL)
    return;

  identifier = meta["identifier$"];

  if (strcmp(meta["tag$"], self->tagged) != 0 ||
      identifier[0] == 0 || strcmp(identifier, self->identifier) == 0 ||
      strlen(identifier) >= MESH_ID_MAX)
    return;

  if (m->state == SWIM_ALIVE)
    addNode(self, m, identifier, meta["v$"]);
  else if (m->state == SWIM_FAULTY)
    removeNode(self, m, identifier);
}
