/**
 * index.js's pin tree and #matchPin, as fix/pattern-shadow has them.
 * pattern.h says what; the comments here say where index.js is particular.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pattern.h"

typedef struct node_s node_t;

typedef struct {
  char *label; /* exact: the pair as the regex matched it; wildcard: the key */
  node_t *child;
} edge_t;

struct node_s {
  void *value; /* f: NULL where no pin ends - held[0] */
  void **held;  /* fl: what was added here, the last added first */
  int heldCount;
  edge_t *exact; /* n */
  int exactCount;
  edge_t *wild;  /* s */
  int wildCount;
};

struct mwm_pins_s {
  node_t root;
};

/* ------------------------------------------------------------- the regex */

/**
 * One match of /([a-zA-Z0-9_-]+)(?:\s+)?:([^,]+)/g from *at on, as exec
 * finds it: a key of word characters and dashes, blanks, a colon, at least
 * one character up to a comma. A start that does not match moves one
 * character on, as the regex does. 0 when there is none left; *at then
 * stands behind the match.
 */
typedef struct {
  const char *whole; /* t[0] */
  size_t wholeLength;
  const char *key;   /* t[1] */
  size_t keyLength;
  const char *value; /* t[2] */
  size_t valueLength;
} match_t;

static int keyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* \s, as far as ASCII goes */
static int blank(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

static int nextMatch(const char **at, match_t *m) {

  const char *start = *at;

  for (; *start != 0; ++start) {

    const char *i = start;
    const char *keyEnd;
    const char *colon;
    const char *end;

    while (keyChar(*i))
      ++i;

    if (i == start)
      continue;

    keyEnd = i;

    while (blank(*i))
      ++i;

    if (*i != ':')
      continue;

    colon = i;
    end = colon + 1;

    while (*end != 0 && *end != ',')
      ++end;

    if (end == colon + 1)
      continue;

    m->whole = start;
    m->wholeLength = (size_t)(end - start);
    m->key = start;
    m->keyLength = (size_t)(keyEnd - start);
    m->value = colon + 1;
    m->valueLength = (size_t)(end - colon - 1);
    *at = end;

    return 1;
  }

  *at = start;
  return 0;
}

static char *copyOf(const char *from, size_t length) {

  char *out = (char *)malloc(length + 1);

  if (out != NULL) {
    memcpy(out, from, length);
    out[length] = 0;
  }

  return out;
}

/* --------------------------------------------------------------- parts */

void mwm_partsInit(mwm_parts_t *parts) { memset(parts, 0, sizeof *parts); }

void mwm_partsFree(mwm_parts_t *parts) {

  for (int i = 0; i < parts->count; ++i)
    free(parts->items[i]);

  free(parts->items);
  memset(parts, 0, sizeof *parts);
}

static int partsTake(mwm_parts_t *parts, char *part) {

  if (part == NULL)
    return 0;

  if (parts->count == parts->room) {

    int room = parts->room > 0 ? parts->room * 2 : 8;
    char **items = (char **)realloc(parts->items, (size_t)room * sizeof *items);

    if (items == NULL) {
      free(part);
      return 0;
    }

    parts->items = items;
    parts->room = room;
  }

  parts->items[parts->count++] = part;
  return 1;
}

int mwm_partsAdd(mwm_parts_t *parts, const char *part) {
  return partsTake(parts, strdup(part));
}

int mwm_partsFromPattern(mwm_parts_t *parts, const char *pattern) {

  const char *at = pattern != NULL ? pattern : "";
  match_t m;

  while (nextMatch(&at, &m))
    if (!partsTake(parts, copyOf(m.whole, m.wholeLength)))
      return 0;

  return 1;
}

/**
 * Number.prototype.toString: the shortest digits that read back as the same
 * double, then plain up to 21 digits before the point and 6 zeros after it,
 * the exponent form beyond.
 */
static void jsNumber(double d, char *into, size_t room) {

  char sci[40];
  char digits[24];
  int count = 0;
  int exponent;
  int n;
  int negative = d < 0;
  char *at;
  char *end = into + room;

  if (isnan(d)) {
    snprintf(into, room, "NaN");
    return;
  }

  if (isinf(d)) {
    snprintf(into, room, "%sInfinity", negative ? "-" : "");
    return;
  }

  if (d == 0) {
    snprintf(into, room, "0");
    return;
  }

  for (int precision = 1; precision <= 17; ++precision) {
    snprintf(sci, sizeof sci, "%.*e", precision - 1, fabs(d));
    if (strtod(sci, NULL) == fabs(d))
      break;
  }

  for (at = sci; *at != 0 && *at != 'e'; ++at)
    if (*at >= '0' && *at <= '9' && count < (int)sizeof digits - 1)
      digits[count++] = *at;

  exponent = *at == 'e' ? atoi(at + 1) : 0;

  while (count > 1 && digits[count - 1] == '0')
    --count;

  digits[count] = 0;
  n = exponent + 1;
  at = into;

#define PUT(c)                                                                 \
  do {                                                                         \
    if (at < end - 1)                                                          \
      *at++ = (c);                                                             \
  } while (0)

  if (negative)
    PUT('-');

  if (count <= n && n <= 21) {
    for (int i = 0; i < count; ++i)
      PUT(digits[i]);
    for (int i = count; i < n; ++i)
      PUT('0');
  } else if (0 < n && n <= 21) {
    for (int i = 0; i < count; ++i) {
      if (i == n)
        PUT('.');
      PUT(digits[i]);
    }
  } else if (-6 < n && n <= 0) {
    PUT('0');
    PUT('.');
    for (int i = 0; i < -n; ++i)
      PUT('0');
    for (int i = 0; i < count; ++i)
      PUT(digits[i]);
  } else {
    char tail[16];

    PUT(digits[0]);
    if (count > 1) {
      PUT('.');
      for (int i = 1; i < count; ++i)
        PUT(digits[i]);
    }
    snprintf(tail, sizeof tail, "e%c%d", n - 1 >= 0 ? '+' : '-', abs(n - 1));
    for (char *t = tail; *t != 0; ++t)
      PUT(*t);
  }

#undef PUT

  *at = 0;
}

META_OWNS char *mwm_jsText(yyjson_mut_val *node) {

  char number[64];

  if (node == NULL)
    return NULL;

  switch (yyjson_mut_get_type(node)) {
  case YYJSON_TYPE_STR:
    return copyOf(yyjson_mut_get_str(node), yyjson_mut_get_len(node));
  case YYJSON_TYPE_BOOL:
    return strdup(yyjson_mut_get_bool(node) ? "true" : "false");
  case YYJSON_TYPE_NUM:
    /* JSON.parse makes a double of every number, a big integer too */
    switch (yyjson_mut_get_subtype(node)) {
    case YYJSON_SUBTYPE_UINT:
      jsNumber((double)yyjson_mut_get_uint(node), number, sizeof number);
      break;
    case YYJSON_SUBTYPE_SINT:
      jsNumber((double)yyjson_mut_get_sint(node), number, sizeof number);
      break;
    default:
      jsNumber(yyjson_mut_get_real(node), number, sizeof number);
      break;
    }
    return strdup(number);
  default:
    return NULL; /* null, objects and arrays are FORBIDDEN in index.js */
  }
}

int mwm_partsFromArgs(mwm_parts_t *parts, json_t args) {

  yyjson_mut_obj_iter it;
  yyjson_mut_val *key;

  if (args._node == NULL || !yyjson_mut_is_obj(args._node))
    return 1;

  yyjson_mut_obj_iter_init(args._node, &it);

  while ((key = yyjson_mut_obj_iter_next(&it)) != NULL) {

    char *text = mwm_jsText(yyjson_mut_obj_iter_get_val(key));
    size_t keyLength = yyjson_mut_get_len(key);
    size_t textLength;
    char *part;

    if (text == NULL)
      continue;

    textLength = strlen(text);
    part = (char *)malloc(keyLength + textLength + 2);

    if (part != NULL) {
      memcpy(part, yyjson_mut_get_str(key), keyLength);
      part[keyLength] = ':';
      memcpy(part + keyLength + 1, text, textLength + 1);
    }

    free(text);

    if (!partsTake(parts, part))
      return 0;
  }

  return 1;
}

int mwm_partsForAct(mwm_parts_t *parts, const char *pattern, json_t args) {
  return mwm_partsFromPattern(parts, pattern) && mwm_partsFromArgs(parts, args);
}

static int byText(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/** [...new Set(parts)].sort() */
static void sortUnique(mwm_parts_t *parts) {

  int kept = 0;

  if (parts->count == 0)
    return;

  qsort(parts->items, (size_t)parts->count, sizeof parts->items[0], byText);

  for (int i = 0; i < parts->count; ++i) {
    if (kept > 0 && strcmp(parts->items[kept - 1], parts->items[i]) == 0) {
      free(parts->items[i]);
      continue;
    }
    parts->items[kept++] = parts->items[i];
  }

  parts->count = kept;
}

/* -------------------------------------------------------------- objects */

META_OWNS json_t mwm_patternObject(const char *pattern) {

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *object = yyjson_mut_obj(doc);
  const char *at = pattern != NULL ? pattern : "";
  match_t m;

  yyjson_mut_doc_set_root(doc, object);

  /* o[t[1]] = t[2]: a key already there keeps its place, takes the value */
  while (nextMatch(&at, &m)) {

    yyjson_mut_val *name = yyjson_mut_strncpy(doc, m.key, m.keyLength);
    yyjson_mut_val *value = yyjson_mut_strncpy(doc, m.value, m.valueLength);

    if (!yyjson_mut_obj_replace(object, name, value))
      yyjson_mut_obj_add(object, name, value);
  }

  return meta_jsonFromMut(doc);
}

META_OWNS char *mwm_patternKey(const char *pattern) {

  mwm_parts_t parts;
  size_t room = 1;
  char *out;
  char *at;

  mwm_partsInit(&parts);
  mwm_partsFromPattern(&parts, pattern);

  /* convertData's c is sorted, not made unique */
  if (parts.count > 0)
    qsort(parts.items, (size_t)parts.count, sizeof parts.items[0], byText);

  for (int i = 0; i < parts.count; ++i)
    room += strlen(parts.items[i]) + 1;

  out = (char *)malloc(room);

  if (out != NULL) {
    at = out;
    *at = 0;
    for (int i = 0; i < parts.count; ++i)
      at += sprintf(at, "%s%s", i != 0 ? "," : "", parts.items[i]);
  }

  mwm_partsFree(&parts);

  return out;
}

/* ----------------------------------------------------------------- tree */

mwm_pins_t *mwm_pinsNew(void) {
  return (mwm_pins_t *)calloc(1, sizeof(mwm_pins_t));
}

static void nodeFree(node_t *node) {

  for (int i = 0; i < node->exactCount; ++i) {
    nodeFree(node->exact[i].child);
    free(node->exact[i].child);
    free(node->exact[i].label);
  }

  for (int i = 0; i < node->wildCount; ++i) {
    nodeFree(node->wild[i].child);
    free(node->wild[i].child);
    free(node->wild[i].label);
  }

  free(node->exact);
  free(node->wild);
  free(node->held);
}

void mwm_pinsFree(mwm_pins_t *pins) {

  if (pins == NULL)
    return;

  nodeFree(&pins->root);
  free(pins);
}

static node_t *childOf(edge_t *edges, int count, const char *label) {

  for (int i = 0; i < count; ++i)
    if (strcmp(edges[i].label, label) == 0)
      return edges[i].child;

  return NULL;
}

static node_t *childMade(edge_t **edges, int *count, const char *label) {

  node_t *child = childOf(*edges, *count, label);
  edge_t *grown;

  if (child != NULL)
    return child;

  grown = (edge_t *)realloc(*edges, (size_t)(*count + 1) * sizeof **edges);

  if (grown == NULL)
    return NULL;

  *edges = grown;
  child = (node_t *)calloc(1, sizeof *child);

  if (child == NULL)
    return NULL;

  grown[*count].label = strdup(label);

  if (grown[*count].label == NULL) {
    free(child);
    return NULL;
  }

  grown[*count].child = child;
  ++*count;

  return child;
}

typedef struct {
  char *p;   /* exact: t[0]; wildcard: t[1] */
  int exact;
} step_t;

/**
 * convertPin's sort, `a.p < b.p ? -1 : ... : 0`: it never answers 1, which
 * V8's sort - a binary insertion sort below 64 entries, asking only `< 0` -
 * takes as a stable sort by p. So does this.
 */
static void sortSteps(step_t *steps, int count) {

  for (int i = 1; i < count; ++i) {

    step_t moving = steps[i];
    int j = i;

    while (j > 0 && strcmp(moving.p, steps[j - 1].p) < 0) {
      steps[j] = steps[j - 1];
      --j;
    }

    steps[j] = moving;
  }
}

/** A pin's steps, sorted as convertPin sorts them; NULL without memory. */
static step_t *stepsOf(const char *pattern, int *made) {

  const char *at = pattern != NULL ? pattern : "";
  step_t *steps;
  int count = 0;
  match_t m;

  *made = 0;

  while (nextMatch(&at, &m))
    ++count;

  steps = (step_t *)calloc((size_t)count + 1, sizeof *steps);

  if (steps == NULL)
    return NULL;

  at = pattern != NULL ? pattern : "";

  while (*made < count && nextMatch(&at, &m)) {

    /* t[2] !== '*': exact as the whole match, a wildcard as the key */
    if (m.valueLength == 1 && m.value[0] == '*') {
      steps[*made].p = copyOf(m.key, m.keyLength);
      steps[*made].exact = 0;
    } else {
      steps[*made].p = copyOf(m.whole, m.wholeLength);
      steps[*made].exact = 1;
    }

    if (steps[*made].p == NULL) {
      for (int i = 0; i < *made; ++i)
        free(steps[i].p);
      free(steps);
      *made = 0;
      return NULL;
    }

    ++*made;
  }

  sortSteps(steps, *made);

  return steps;
}

static void stepsFree(step_t *steps, int count) {

  for (int i = 0; i < count; ++i)
    free(steps[i].p);

  free(steps);
}

/** Takes value out of what a node holds; f is the last added of the rest. */
static void unhold(node_t *node, void *value) {

  int kept = 0;

  for (int i = 0; i < node->heldCount; ++i)
    if (node->held[i] != value)
      node->held[kept++] = node->held[i];

  node->heldCount = kept;
  node->value = kept > 0 ? node->held[0] : NULL;
}

int mwm_pinsAdd(mwm_pins_t *pins, const char *pattern, void *value) {

  int count;
  step_t *steps = stepsOf(pattern, &count);
  node_t *before = &pins->root;
  void **grown;

  if (steps == NULL)
    return 0;

  for (int i = 0; i < count && before != NULL; ++i)
    before = steps[i].exact
                 ? childMade(&before->exact, &before->exactCount, steps[i].p)
                 : childMade(&before->wild, &before->wildCount, steps[i].p);

  stepsFree(steps, count);

  if (before == NULL)
    return 0;

  /* before.f = f; before.fl.unshift(f) - once each, the newest first */
  unhold(before, value);
  grown = (void **)realloc(before->held,
                           (size_t)(before->heldCount + 1) * sizeof *grown);

  if (grown == NULL)
    return 0;

  memmove(grown + 1, grown, (size_t)before->heldCount * sizeof *grown);
  grown[0] = value;
  before->held = grown;
  ++before->heldCount;
  before->value = value;

  return 1;
}

static int edgeAt(const edge_t *edges, int count, const node_t *child) {

  for (int i = 0; i < count; ++i)
    if (edges[i].child == child)
      return i;

  return -1;
}

/** Drops the i-th edge and its empty child. */
static void edgeDrop(edge_t *edges, int *count, int i) {

  node_t *child = edges[i].child;

  free(edges[i].label);
  free(child->exact);
  free(child->wild);
  free(child->held);
  free(child);
  memmove(edges + i, edges + i + 1, (size_t)(*count - i - 1) * sizeof *edges);
  --*count;
}

int mwm_pinsRemove(mwm_pins_t *pins, const char *pattern, void *value) {

  int count;
  step_t *steps = stepsOf(pattern, &count);
  node_t **path;
  int depth = 0;

  if (steps == NULL)
    return 0;

  path = (node_t **)calloc((size_t)count + 1, sizeof *path);

  if (path == NULL) {
    stepsFree(steps, count);
    return 0;
  }

  path[0] = &pins->root;

  for (int i = 0; i < count; ++i) {

    node_t *at = path[depth];
    node_t *next = steps[i].exact ? childOf(at->exact, at->exactCount, steps[i].p)
                                  : childOf(at->wild, at->wildCount, steps[i].p);

    if (next == NULL) {
      free(path);
      stepsFree(steps, count);
      return 0;
    }

    path[++depth] = next;
  }

  unhold(path[depth], value);

  /* #dropPin: up from the leaf, every node with neither a pin nor a branch */
  for (int i = depth; i > 0; --i) {

    node_t *child = path[i];
    node_t *parent = path[i - 1];

    if (child->value != NULL || child->exactCount > 0 || child->wildCount > 0)
      break;

    {
      int exact = edgeAt(parent->exact, parent->exactCount, child);

      if (exact >= 0)
        edgeDrop(parent->exact, &parent->exactCount, exact);
      else {
        int wild = edgeAt(parent->wild, parent->wildCount, child);

        if (wild >= 0)
          edgeDrop(parent->wild, &parent->wildCount, wild);
      }
    }
  }

  free(path);
  stepsFree(steps, count);

  return 1;
}

/* ------------------------------------------------------------- matching */

typedef struct {
  char **parts;  /* sorted, unique */
  int partCount;
  char **keys;   /* o.split(':')[0] of each, unique, sorted */
  int *from;     /* the first and behind the last part of a key */
  int *to;
  int *keyOf;    /* the key of each part */
  int keyCount;
  void *best;
  int bestPairs;
  int bestExact;
} walk_t;

/**
 * #matchPin's walk: a node with a pin is taken when it has more pairs, or as
 * many and more exact ones, than the best so far - so of equals the first one
 * walked to stays. From every node every key of the message is tried, in
 * order: its parts down the exact branches, then the wildcard branch.
 */
static void walk(walk_t *w, const node_t *node, int pairs, int exact) {

  if (node->value != NULL &&
      (pairs > w->bestPairs || (pairs == w->bestPairs && exact > w->bestExact))) {
    w->best = node->value;
    w->bestPairs = pairs;
    w->bestExact = exact;
  }

  for (int k = 0; k < w->keyCount; ++k) {

    const node_t *child;

    if (node->exactCount > 0)
      for (int i = w->from[k]; i < w->to[k]; ++i)
        if (w->keyOf[i] == k &&
            (child = childOf(node->exact, node->exactCount, w->parts[i])) != NULL)
          walk(w, child, pairs + 1, exact + 1);

    if (node->wildCount > 0 &&
        (child = childOf(node->wild, node->wildCount, w->keys[k])) != NULL)
      walk(w, child, pairs + 1, exact);
  }
}

static size_t keyLength(const char *part) {

  const char *colon = strchr(part, ':');

  return colon != NULL ? (size_t)(colon - part) : strlen(part);
}

static int byKey(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

void *mwm_pinsMatch(const mwm_pins_t *pins, mwm_parts_t *parts) {

  walk_t w;
  int n;

  if (pins == NULL)
    return NULL;

  sortUnique(parts);
  n = parts->count;

  memset(&w, 0, sizeof w);
  w.parts = parts->items;
  w.partCount = n;
  w.bestPairs = -1;
  w.bestExact = -1;

  if (n > 0) {

    w.keys = (char **)calloc((size_t)n, sizeof *w.keys);
    w.from = (int *)calloc((size_t)n, sizeof *w.from);
    w.to = (int *)calloc((size_t)n, sizeof *w.to);
    w.keyOf = (int *)calloc((size_t)n, sizeof *w.keyOf);

    if (w.keys == NULL || w.from == NULL || w.to == NULL || w.keyOf == NULL) {
      free(w.keys);
      free(w.from);
      free(w.to);
      free(w.keyOf);
      return NULL;
    }

    for (int i = 0; i < n; ++i) {

      size_t length = keyLength(parts->items[i]);
      int known = 0;

      for (int k = 0; k < w.keyCount; ++k)
        if (strlen(w.keys[k]) == length &&
            strncmp(w.keys[k], parts->items[i], length) == 0) {
          known = 1;
          break;
        }

      if (!known)
        w.keys[w.keyCount++] = copyOf(parts->items[i], length);
    }

    qsort(w.keys, (size_t)w.keyCount, sizeof w.keys[0], byKey);

    /*
     * The parts of one key, in their sorted order: those that begin with
     * `key:` stand together; a part without a colon is its own key and may
     * stand apart from them.
     */
    for (int k = 0; k < w.keyCount; ++k) {

      size_t length = strlen(w.keys[k]);

      w.from[k] = n;
      w.to[k] = n;

      for (int i = 0; i < n; ++i)
        if (keyLength(parts->items[i]) == length &&
            strncmp(parts->items[i], w.keys[k], length) == 0) {
          if (w.from[k] == n)
            w.from[k] = i;
          w.to[k] = i + 1;
          w.keyOf[i] = k;
        }
    }
  }

  walk(&w, &pins->root, 0, 0);

  for (int k = 0; k < w.keyCount; ++k)
    free(w.keys[k]);

  free(w.keys);
  free(w.from);
  free(w.to);
  free(w.keyOf);

  return w.best;
}
