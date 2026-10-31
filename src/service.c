/**
 * transfer.js's handleRequest and index.js's matching, for one line at a time.
 * service.h says what; the comments here say where microwizard is particular.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service.h"

typedef struct {
  char *pattern;
  mwm_handler_t handler;
  int inline_; /* never parks: mwm_addInline */
} route_t;

/* every handler in the order added, and the tree they are matched in */
static route_t[] routes;
static mwm_pins_t *pins;
static int offline;

static int addRoute(const char *pattern, mwm_handler_t handler, int inline_) {

  route_t route;
  mwm_parts_t pairs;
  int count;

  mwm_partsInit(&pairs);
  mwm_partsFromPattern(&pairs, pattern);
  count = pairs.count;
  mwm_partsFree(&pairs);

  if (count == 0 || handler == NULL)
    return 0;

  if (pins == NULL && (pins = mwm_pinsNew()) == NULL)
    return 0;

  route.pattern = strdup(pattern);
  route.handler = handler;
  route.inline_ = inline_;

  if (route.pattern == NULL)
    return 0;

  if (!routes.push(route)) {
    free(route.pattern);
    return 0;
  }

  /* the function pointer is the node's value: the one added last holds it */
  return mwm_pinsAdd(pins, pattern, (void *)handler);
}

int mwm_add(const char *pattern, mwm_handler_t handler) {
  return addRoute(pattern, handler, 0);
}

int mwm_addInline(const char *pattern, mwm_handler_t handler) {
  return addRoute(pattern, handler, 1);
}

/** Whether the handler a message matched was added as one that never parks. */
static int runsInline(mwm_handler_t handler) {

  for (size_t i = routes.count; i > 0; --i)
    if (routes.items[i - 1].handler == handler)
      return routes.items[i - 1].inline_;

  return 0;
}

void mwm_clear(void) {

  for (r in routes)
    free(r->pattern);

  routes.release();
  mwm_pinsFree(pins);
  pins = NULL;
}

int mwm_handlerCount(void) { return (int)routes.count; }

const char *mwm_handlerPattern(int i) {
  return i >= 0 && i < (int)routes.count ? routes.items[i].pattern : NULL;
}

mwm_handler_t mwm_handlerAt(int i) {
  return i >= 0 && i < (int)routes.count ? routes.items[i].handler : NULL;
}

void mwm_goOffline(void) { offline = 1; }

int mwm_isOffline(void) { return offline; }

mwm_handler_t mwm_findHandler(mwm_parts_t *parts) {
  return (mwm_handler_t)mwm_pinsMatch(pins, parts);
}

/** act(args): the primitive fields of args are the message. */
static mwm_handler_t findForArgs(json_t args) {

  mwm_parts_t parts;
  mwm_handler_t handler;

  mwm_partsInit(&parts);
  mwm_partsFromArgs(&parts, args);
  handler = mwm_findHandler(&parts);
  mwm_partsFree(&parts);

  return handler;
}

/** actE(p, d): the pairs of p, as convertData finds them, are the message. */
static mwm_handler_t findForPattern(const char *pattern) {

  mwm_parts_t parts;
  mwm_handler_t handler;

  mwm_partsInit(&parts);
  mwm_partsFromPattern(&parts, pattern);
  handler = mwm_findHandler(&parts);
  mwm_partsFree(&parts);

  return handler;
}

/**
 * The answer line: id and sync copied from the request, then res or error.
 * error gets name "Error" when the handler did not say, as transfer.js does
 * with `err.name || 'Error'`.
 */
static char *answerLine(json_t asked, int thrown, json_t value, int stacked) {

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *out = yyjson_mut_obj(doc);
  json_t id = asked.id;
  json_t sync = asked.sync;
  char *text;

  yyjson_mut_doc_set_root(doc, out);

  yyjson_mut_obj_add(out, yyjson_mut_str(doc, "id"),
                     id._node != NULL ? yyjson_mut_val_mut_copy(doc, id._node)
                                      : yyjson_mut_null(doc));
  yyjson_mut_obj_add(out, yyjson_mut_str(doc, "k"), yyjson_mut_str(doc, "res"));

  if (sync._node != NULL)
    yyjson_mut_obj_add(out, yyjson_mut_str(doc, "sync"),
                       yyjson_mut_val_mut_copy(doc, sync._node));

  if (thrown) {

    yyjson_mut_val *error = value._node != NULL &&
                                    yyjson_mut_is_obj(value._node)
                                ? yyjson_mut_val_mut_copy(doc, value._node)
                                : yyjson_mut_obj(doc);

    if (yyjson_mut_obj_get(error, "message") == NULL)
      yyjson_mut_obj_add(error, yyjson_mut_str(doc, "message"),
                         yyjson_mut_str(doc, ""));
    if (yyjson_mut_obj_get(error, "name") == NULL)
      yyjson_mut_obj_add(error, yyjson_mut_str(doc, "name"),
                         yyjson_mut_str(doc, "Error"));

    /**
     * transfer.js sends err.stack, and the caller's Error takes it over - so
     * that is what an inspect of the rejection shows. A handler here has no
     * JS stack; it gets the first line one would have, and where it was.
     */
    if (stacked && yyjson_mut_obj_get(error, "stack") == NULL) {

      char stack[512];
      json_t args = asked.args;
      const char *where = !args.isNothing() ? "args" : asked.p.text();

      snprintf(stack, sizeof stack, "%s: %s\n    at mwm-node (%s)",
               yyjson_mut_get_str(yyjson_mut_obj_get(error, "name")),
               yyjson_mut_get_str(yyjson_mut_obj_get(error, "message")) != NULL
                   ? yyjson_mut_get_str(yyjson_mut_obj_get(error, "message"))
                   : "",
               where);
      yyjson_mut_obj_add(error, yyjson_mut_str(doc, "stack"),
                         yyjson_mut_strcpy(doc, stack));
    }

    yyjson_mut_obj_add(out, yyjson_mut_str(doc, "error"), error);

  } else if (value._node != NULL) {
    yyjson_mut_obj_add(out, yyjson_mut_str(doc, "res"),
                       yyjson_mut_val_mut_copy(doc, value._node));
  }

  text = yyjson_mut_write(doc, 0, NULL);
  yyjson_mut_doc_free(doc);

  return text;
}

/**
 * The answer line of a call that did not throw, written straight out: no
 * document of its own, no copies of id, sync and res into one - which was
 * half of what answering cost, in allocations. The same text as answerLine
 * gives (id, k, sync, res in that order, compact), for the shapes calls
 * have: an id that is a string needing no escapes or a number, a sync that
 * is true or false. Anything else is NULL, and answerLine does it.
 */
static int plainString(const char *text, size_t length) {

  for (size_t i = 0; i < length; ++i) {

    unsigned char c = (unsigned char)text[i];

    if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7f)
      return 0;
  }

  return 1;
}

/* `length` bytes of `what` at out + used; snprintf was a third of the gain */
static size_t put(char *out, size_t used, const char *what, size_t length) {

  memcpy(out + used, what, length);

  return used + length;
}

static char *answerFast(json_t asked, json_t value) {

  yyjson_mut_val *id = asked.id._node;
  yyjson_mut_val *sync = asked.sync._node;
  char number[32];
  const char *idText;
  size_t idLength;
  int quoted;
  char *res = NULL;
  size_t resLength = 0;
  size_t room;
  char *out;

  if (id == NULL)
    return NULL;

  if (yyjson_mut_is_str(id)) {
    idText = yyjson_mut_get_str(id);
    idLength = yyjson_mut_get_len(id);
    quoted = 1;
    if (!plainString(idText, idLength))
      return NULL;
  } else if (yyjson_mut_is_uint(id) || yyjson_mut_is_sint(id)) {
    idLength = (size_t)snprintf(number, sizeof number, yyjson_mut_is_uint(id) ? "%llu" : "%lld",
                                yyjson_mut_is_uint(id) ? (unsigned long long)yyjson_mut_get_uint(id)
                                                       : (unsigned long long)yyjson_mut_get_sint(id));
    idText = number;
    quoted = 0;
  } else
    return NULL;

  if (sync != NULL && !yyjson_mut_is_bool(sync))
    return NULL;

  if (value._node != NULL) {
    res = yyjson_mut_val_write(value._node, 0, &resLength);
    if (res == NULL)
      return NULL;
  }

  room = idLength + resLength + 64;
  out = (char *)malloc(room);

  if (out != NULL) {

    const char *syncText = sync == NULL                ? ""
                           : yyjson_mut_get_bool(sync) ? ",\"sync\":true"
                                                       : ",\"sync\":false";
    size_t used = put(out, 0, "{\"id\":", 6);

    if (quoted)
      used = put(out, used, "\"", 1);
    used = put(out, used, idText, idLength);
    if (quoted)
      used = put(out, used, "\"", 1);
    used = put(out, used, ",\"k\":\"res\"", 10);
    used = put(out, used, syncText, strlen(syncText));
    if (res != NULL) {
      used = put(out, used, ",\"res\":", 7);
      used = put(out, used, res, resLength);
    }
    used = put(out, used, "}", 2); /* and the terminator */
  }

  free(res);

  return out;
}

static char *answerOf(json_t asked, int thrown, json_t value) {

  char *out = thrown ? NULL : answerFast(asked, value);

  return out != NULL ? out : answerLine(asked, thrown, value, 1);
}

META_OWNS char *mwm_respond(const char *line, size_t length) {

  json_t asked = meta_toJSONn(line, length);
  defer asked.release();
  const char *k;
  int isSync;
  mwm_handler_t handler;
  mwm_answer_t answer = {0, {NULL, NULL, NULL, "", NULL, 0, NULL}};
  char *out;

  /* the parser's error closes the connection in tcp.js; main.c does that */
  if (asked._refused != NULL)
    return NULL;

  if (asked.h.number() == 1)
    return strdup("{\"h\":2}");

  k = asked.k;

  if (strcmp(k, "a") != 0 && strcmp(k, "aE") != 0)
    return NULL; /* {input, error: 'unknown method'} has no sync and is not sent */

  /* sync false: run it, answer nothing */
  isSync = asked.sync.isNothing() || asked.sync.truth();

  if (offline) {

    json_t error = {message: "retry_later_err_overload", name: "Error"};

    /* exactly transfer.js's: message and name, no stack */
    out = answerLine(asked, 1, error, 0);
    error.release();

    return isSync ? out : (free(out), (char *)NULL);
  }

  if (strcmp(k, "a") == 0) {
    handler = findForArgs(asked.args);
    if (handler != NULL)
      answer = handler(asked.args);
  } else {
    handler = findForPattern(asked.p.text());
    if (handler != NULL)
      answer = handler(asked.d);
  }

  /* no handler: index.js logs no-target and resolves undefined - no res */
  out = answerOf(asked, answer.thrown, answer.value);
  answer.value.release();

  if (!isSync) {
    free(out);
    return NULL;
  }

  return out;
}

/**
 * Too many calls running: refused without a handler. microwizard has no
 * limit of its own; a service that has one throws an error whose
 * details.message is retry_later_err_overload, which is the only overload
 * balance.js counts - it opens the target and tries the call again, on
 * another target, 100 ms later. The closing answer above, without details,
 * would reach the caller as an ordinary error instead.
 */
META_OWNS char *mwm_refuse(const char *line, size_t length) {

  json_t asked = meta_toJSONn(line, length);
  defer asked.release();
  const char *k;
  char *out;

  if (asked._refused != NULL)
    return NULL;

  if (asked.h.number() == 1)
    return strdup("{\"h\":2}");

  k = asked.k;

  if (strcmp(k, "a") != 0 && strcmp(k, "aE") != 0)
    return NULL;

  if (!asked.sync.isNothing() && !asked.sync.truth())
    return NULL;

  json_t error = {message: "retry_later_err_overload", name: "Error",
                  details: {message: "retry_later_err_overload"}};

  out = answerLine(asked, 1, error, 0);
  error.release();

  return out;
}

mwm_inline_t mwm_respondInline(const char *line, size_t length, long *running,
                               long max) {

  json_t asked = meta_toJSONn(line, length);
  defer asked.release();
  const char *k;
  int isSync;
  mwm_handler_t handler;
  json_t args;
  mwm_answer_t answer = {0, {NULL, NULL, NULL, "", NULL, 0, NULL}};
  char *out;

  if (asked._refused != NULL)
    return (mwm_inline_t){MWM_BROKEN, NULL};

  if (asked.h.number() == 1)
    return (mwm_inline_t){MWM_ANSWERED, strdup("{\"h\":2}")};

  k = asked.k;

  if (strcmp(k, "a") != 0 && strcmp(k, "aE") != 0)
    return (mwm_inline_t){MWM_ANSWERED, NULL};

  isSync = asked.sync.isNothing() || asked.sync.truth();

  /* going offline: mwm_respond says it, and runs nothing */
  if (offline)
    return (mwm_inline_t){MWM_ANSWERED, mwm_respond(line, length)};

  if (strcmp(k, "a") == 0) {
    args = asked.args;
    handler = findForArgs(args);
  } else {
    args = asked.d;
    handler = findForPattern(asked.p.text());
  }

  if (handler != NULL && !runsInline(handler))
    return (mwm_inline_t){MWM_LATER, NULL};

  /* the gate, as for a call in a task of its own */
  if (__atomic_add_fetch(running, 1, __ATOMIC_ACQ_REL) > max) {
    __atomic_sub_fetch(running, 1, __ATOMIC_ACQ_REL);
    return (mwm_inline_t){MWM_ANSWERED, mwm_refuse(line, length)};
  }

  /* no handler: index.js logs no-target and resolves undefined - no res */
  if (handler != NULL)
    answer = handler(args);

  out = answerOf(asked, answer.thrown, answer.value);
  answer.value.release();

  __atomic_sub_fetch(running, 1, __ATOMIC_ACQ_REL);

  if (!isSync) {
    free(out);
    out = NULL;
  }

  return (mwm_inline_t){MWM_ANSWERED, out};
}
