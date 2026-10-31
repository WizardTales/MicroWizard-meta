/**
 * pattern.c: which pin a message reaches.
 *
 * First the cases of test/pattern.test.js from microwizard fix/pattern-shadow,
 * one for one - each `reaches(mc, pin, data)` an act through the tree, the
 * same pins in the same order. Then the same rules through service.c (calls
 * that come in) and through mwm_act/mwm_actE without a mesh. Then the trace
 * of test/harness/pattern-trace.js: pin sets and messages, fixed and seeded
 * random, with the winner microwizard's own matcher picked; every one must
 * be ours too.
 *
 *   pattern_test [fixtures]     the cases, then fixtures/pattern-trace.txt
 *   pattern_test - < trace      the cases, then a trace from stdin - how
 *                               test/run.sh compares a fresh random trace
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <meta_json.h>

#include "client.h"
#include "pattern.h"
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

/* ------------------------------------------------- a node, as the test */

#define MOST 16

/** `node(...pins)`: a tree whose every pin answers with itself. */
typedef struct {
  mwm_pins_t *pins;
  const char *names[MOST];
  int count;
} mc_t;

static void node(mc_t *mc, const char *const *pins) {

  mc->pins = mwm_pinsNew();
  mc->count = 0;

  for (; *pins != NULL && mc->count < MOST; ++pins) {
    mc->names[mc->count] = *pins;
    mwm_pinsAdd(mc->pins, *pins, (void *)mc->names[mc->count]);
    ++mc->count;
  }
}

/** `mc.act(pin, data)`: the pin that answers, or NULL for no-target. */
static const char *reaches(mc_t *mc, const char *pin, const char *data) {

  mwm_parts_t parts;
  json_t args = data != NULL ? meta_toJSON(data)
                             : (json_t){NULL, NULL, NULL, "", NULL, 0, NULL};
  const char *got;

  mwm_partsInit(&parts);
  mwm_partsForAct(&parts, pin, args);
  got = (const char *)mwm_pinsMatch(mc->pins, &parts);
  mwm_partsFree(&parts);
  args.release();

  return got;
}

/** `mc.actE(pin, {})` and `mc.find(pin)`: the pattern alone. */
static const char *reachesE(mc_t *mc, const char *pin) {

  mwm_parts_t parts;
  const char *got;

  mwm_partsInit(&parts);
  mwm_partsFromPattern(&parts, pin);
  got = (const char *)mwm_pinsMatch(mc->pins, &parts);
  mwm_partsFree(&parts);

  return got;
}

static int same(const char *got, const char *want) {
  return (got == NULL && want == NULL) ||
         (got != NULL && want != NULL && strcmp(got, want) == 0);
}

#define REACHES(mc, pin, data, want)                                           \
  do {                                                                         \
    const char *got_ = reaches(mc, pin, data);                                 \
    CHECK(same(got_, want), "%s %s: got %s, want %s", pin,                     \
          (data) != NULL ? (const char *)(data) : "", got_ ? got_ : "undefined", \
          (want) ? (const char *)(want) : "undefined");                         \
  } while (0)

static void done(mc_t *mc) { mwm_pinsFree(mc->pins); }

/* -------------------------------------------- test/pattern.test.js */

static void patternTestJs(void) {

  mc_t mc;

  /* an exact pin of another service hides no wildcard pin */
  {
    static const char *const pins[] = {"role:mwmobserver,cmd:ping",
                                       "role:mwmtest,cmd:*", NULL};
    node(&mc, pins);
    REACHES(&mc, "role:mwmtest,cmd:ping", NULL, "role:mwmtest,cmd:*");
    REACHES(&mc, "role:mwmtest,cmd:fail", NULL, "role:mwmtest,cmd:*");
    REACHES(&mc, "role:mwmobserver,cmd:ping", NULL, "role:mwmobserver,cmd:ping");
    REACHES(&mc, "role:mwmobserver,cmd:fail", NULL, NULL);
    done(&mc);
  }

  /* the same, in the order core services use */
  {
    static const char *const pins[] = {"service:inventory,command:create",
                                       "service:user,command:*",
                                       "service:user,command:get,asset:quota",
                                       NULL};
    node(&mc, pins);
    REACHES(&mc, "service:user,command:create", NULL, "service:user,command:*");
    REACHES(&mc, "service:user,command:get,asset:quota", NULL,
            "service:user,command:get,asset:quota");
    REACHES(&mc, "service:user,command:get,asset:other", NULL,
            "service:user,command:*");
    REACHES(&mc, "service:inventory,command:create", NULL,
            "service:inventory,command:create");
    REACHES(&mc, "service:inventory,command:list", NULL, NULL);
    done(&mc);
  }

  /* an exact pin beats a wildcard pin for the same message */
  {
    static const char *const first[] = {"role:a,cmd:ping", "role:a,cmd:*", NULL};
    static const char *const second[] = {"role:a,cmd:*", "role:a,cmd:ping", NULL};
    const char *const *orders[] = {first, second};

    for (int i = 0; i < 2; ++i) {
      node(&mc, orders[i]);
      REACHES(&mc, "role:a,cmd:ping", NULL, "role:a,cmd:ping");
      REACHES(&mc, "role:a,cmd:pong", NULL, "role:a,cmd:*");
      done(&mc);
    }
  }

  /* more pairs win, exact or wildcard */
  {
    static const char *const pins[] = {"a:1", "a:*,b:2", "role:x",
                                       "cmd:*,role:x", NULL};
    node(&mc, pins);
    REACHES(&mc, "a:1,b:2", NULL, "a:*,b:2");
    REACHES(&mc, "a:1,b:3", NULL, "a:1");
    REACHES(&mc, "role:x,cmd:go", NULL, "cmd:*,role:x");
    REACHES(&mc, "role:x", NULL, "role:x");
    done(&mc);
  }

  /* pairs a pin does not name are passed over */
  {
    static const char *const pins[] = {"role:a,cmd:*", "cmd:ping,role:b", NULL};
    node(&mc, pins);
    REACHES(&mc, "role:a,cmd:ping,x:1", NULL, "role:a,cmd:*");
    REACHES(&mc, "role:a,cmd:ping", "{\"aa\":1,\"zz\":2}", "role:a,cmd:*");
    REACHES(&mc, "role:b,cmd:ping", "{\"aa\":1}", "cmd:ping,role:b");
    done(&mc);
  }

  /* a tie goes to the pin exact on the first key */
  {
    static const char *const pins[] = {"a:*,b:1", "a:1,b:*", NULL};
    node(&mc, pins);
    REACHES(&mc, "a:1,b:1", NULL, "a:1,b:*");
    done(&mc);
  }

  /* keys that sort around the colon match as well */
  {
    static const char *const pins[] = {"a:*,a-b:1", "a1:*,b:2", NULL};
    node(&mc, pins);
    REACHES(&mc, "a:x,a-b:1", NULL, "a:*,a-b:1");
    REACHES(&mc, "a1:x,b:2", NULL, "a1:*,b:2");
    done(&mc);
  }

  /* find and actE resolve the same way */
  {
    static const char *const pins[] = {"role:mwmobserver,cmd:ping",
                                       "role:mwmtest,cmd:*", NULL};
    const char *got;

    node(&mc, pins);
    got = reachesE(&mc, "role:mwmtest,cmd:ping");
    CHECK(same(got, "role:mwmtest,cmd:*"), "actE: got %s", got ? got : "undefined");
    done(&mc);
  }
}

/* ------------------------------------------------- removing (dead-pin) */

static void removing(void) {

  mwm_pins_t *pins = mwm_pinsNew();
  static const char *wide = "role:a,cmd:*";
  static const char *here = "here";
  static const char *remote = "remote";
  static const char *deep = "deep";
  const char *got;

  mwm_pinsAdd(pins, wide, (void *)wide);
  mwm_pinsAdd(pins, "role:a,cmd:x", (void *)remote);

  {
    mc_t mc = {pins, {NULL}, 0};

    got = reachesE(&mc, "role:a,cmd:x");
    CHECK(same(got, remote), "before: %s", got ? got : "undefined");

    /* the last target gone: through to the wider pin */
    CHECK(mwm_pinsRemove(pins, "cmd:x,role:a", (void *)remote), "removed");
    got = reachesE(&mc, "role:a,cmd:x");
    CHECK(same(got, wide), "after: %s", got ? got : "undefined");

    /* back again */
    mwm_pinsAdd(pins, "role:a,cmd:x", (void *)remote);
    got = reachesE(&mc, "role:a,cmd:x");
    CHECK(same(got, remote), "back: %s", got ? got : "undefined");

    /* a handler here on the same pin, added before: it holds the node again */
    mwm_pinsRemove(pins, "role:a,cmd:x", (void *)remote);
    mwm_pinsAdd(pins, "role:a,cmd:x", (void *)here);
    mwm_pinsAdd(pins, "role:a,cmd:x", (void *)remote);
    mwm_pinsAdd(pins, "role:a,cmd:x", (void *)remote);
    got = reachesE(&mc, "role:a,cmd:x");
    CHECK(same(got, remote), "over here: %s", got ? got : "undefined");
    mwm_pinsRemove(pins, "role:a,cmd:x", (void *)remote);
    got = reachesE(&mc, "role:a,cmd:x");
    CHECK(same(got, here), "here stays: %s", got ? got : "undefined");

    /* pruned: nothing left of role:b,cmd:x,v:1, nothing matches */
    mwm_pinsAdd(pins, "role:b,cmd:x,v:1", (void *)deep);
    mwm_pinsRemove(pins, "role:b,cmd:x,v:1", (void *)deep);
    got = reachesE(&mc, "role:b,cmd:x,v:1");
    CHECK(got == NULL, "pruned: %s", got ? got : "undefined");
    CHECK(!mwm_pinsRemove(pins, "role:b,cmd:x,v:1", (void *)deep),
          "a pruned pin is not in the tree");
    CHECK(!mwm_pinsRemove(pins, "role:nobody", (void *)deep), "never was");
  }

  mwm_pinsFree(pins);
}

/* ------------------------------------------ the same through service.c */

static mwm_answer_t observer(json_t args) {
  json_t res = {who: "role:mwmobserver,cmd:ping"};
  return (mwm_answer_t){0, res};
}

static mwm_answer_t tester(json_t args) {
  json_t res = {who: "role:mwmtest,cmd:*"};
  return (mwm_answer_t){0, res};
}

static mwm_answer_t starA(json_t args) {
  json_t res = {who: "a:*,b:1"};
  return (mwm_answer_t){0, res};
}

static mwm_answer_t starB(json_t args) {
  json_t res = {who: "a:1,b:*"};
  return (mwm_answer_t){0, res};
}

static void answers(const char *in, const char *want, int line) {

  char *got = mwm_respond(in, strlen(in));

  ++checked;

  if ((want == NULL) != (got == NULL) ||
      (want != NULL && strcmp(want, got) != 0)) {
    ++failures;
    fprintf(stderr, "FAIL line %d: %s\n  got  %s\n  want %s\n", line, in,
            got != NULL ? got : "(nothing)", want != NULL ? want : "(nothing)");
  }

  free(got);
}

#define ANSWERS(in, want) answers(in, want, __LINE__)

static void throughService(void) {

  mwm_add("role:mwmobserver,cmd:ping", observer);
  mwm_add("role:mwmtest,cmd:*", tester);
  mwm_add("a:1,b:*", starB);
  mwm_add("a:*,b:1", starA);

  ANSWERS("{\"id\":\"1\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"mwmtest\","
          "\"cmd\":\"ping\"}}",
          "{\"id\":\"1\",\"k\":\"res\",\"sync\":true,\"res\":{\"who\":"
          "\"role:mwmtest,cmd:*\"}}");
  ANSWERS("{\"id\":\"2\",\"k\":\"aE\",\"sync\":true,\"p\":\"role:mwmtest,cmd:ping\","
          "\"d\":{}}",
          "{\"id\":\"2\",\"k\":\"res\",\"sync\":true,\"res\":{\"who\":"
          "\"role:mwmtest,cmd:*\"}}");
  ANSWERS("{\"id\":\"3\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":"
          "\"mwmobserver\",\"cmd\":\"fail\"}}",
          "{\"id\":\"3\",\"k\":\"res\",\"sync\":true}");

  /* a tie: not the one added last, the one exact on a */
  ANSWERS("{\"id\":\"4\",\"k\":\"a\",\"sync\":true,\"args\":{\"a\":1,\"b\":1}}",
          "{\"id\":\"4\",\"k\":\"res\",\"sync\":true,\"res\":{\"who\":"
          "\"a:1,b:*\"}}");
  ANSWERS("{\"id\":\"5\",\"k\":\"aE\",\"sync\":true,\"p\":\"b:1,a:1\"}",
          "{\"id\":\"5\",\"k\":\"res\",\"sync\":true,\"res\":{\"who\":"
          "\"a:1,b:*\"}}");

  /* and without a mesh, mwm_act and mwm_actE find the same */
  {
    json_t args = meta_toJSON("{\"b\":\"1\"}");
    json_t none = {NULL, NULL, NULL, "", NULL, 0, NULL};
    mwm_settled_t got = mwm_act("a:1", args);

    CHECK(!got.rejected &&
              strcmp(got.value.who, "a:1,b:*") == 0,
          "mwm_act a:1 {b:1}: %s", got.value.who.text());
    got.value.release();

    got = mwm_actE("role:mwmtest,cmd:ping", none);
    CHECK(!got.rejected &&
              strcmp(got.value.who, "role:mwmtest,cmd:*") == 0,
          "mwm_actE: %s", got.value.who.text());
    got.value.release();
    args.release();
  }

  mwm_clear();
}

/* ----------------------------------------------------------- numbers */

static void numbers(void) {

  static const char *const cases[][2] = {
      {"1", "1"},           {"1.0", "1"},
      {"-0", "0"},          {"1.5", "1.5"},
      {"0.1", "0.1"},       {"100", "100"},
      {"1e21", "1e+21"},    {"1e20", "100000000000000000000"},
      {"0.000001", "0.000001"}, {"0.0000001", "1e-7"},
      {"123.456", "123.456"}, {"12345678901234567890", "12345678901234567000"},
      {"-2.5e-10", "-2.5e-10"}, {"9007199254740993", "9007199254740992"},
      {"true", "true"},     {"\"x y\"", "x y"},
  };

  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {

    json_t value = meta_toJSON(cases[i][0]);
    char *text = mwm_jsText(value._node);

    CHECK(text != NULL && strcmp(text, cases[i][1]) == 0,
          "`${%s}`: got %s, want %s", cases[i][0], text ? text : "(none)",
          cases[i][1]);
    free(text);
    value.release();
  }

  {
    json_t value = meta_toJSON("null");
    char *text = mwm_jsText(value._node);

    CHECK(text == NULL, "null is no part");
    free(text);
    value.release();
  }
}

/* -------------------------------------------------------------- trace */

#define LINE 8192

static void trace(FILE *in, const char *name) {

  char line[LINE];
  mwm_pins_t *pins = NULL;
  int pinCount = 0;
  int lineNo = 0;
  int messages = 0;
  int cases = 0;
  int before = failures;
  char caseName[32] = "";

  while (fgets(line, sizeof line, in) != NULL) {

    size_t length = strlen(line);
    char *fields[4] = {NULL, NULL, NULL, NULL};
    int count = 0;
    char *at;

    ++lineNo;

    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
      line[--length] = 0;

    if (strncmp(line, "case ", 5) == 0) {
      mwm_pinsFree(pins);
      pins = mwm_pinsNew();
      pinCount = 0;
      ++cases;
      snprintf(caseName, sizeof caseName, "%s", line + 5);
      continue;
    }

    if (strncmp(line, "pin ", 4) == 0) {
      /* the value is the index + 1: NULL stays no pin */
      mwm_pinsAdd(pins, line + 4, (void *)(intptr_t)(pinCount + 1));
      ++pinCount;
      continue;
    }

    if (strncmp(line, "act ", 4) != 0 && strncmp(line, "actE ", 5) != 0)
      continue;

    for (at = strchr(line, ' ') + 1, fields[count++] = at; *at != 0 && count < 4; ++at)
      if (*at == '\t') {
        *at = 0;
        fields[count++] = at + 1;
      }

    {
      int want = atoi(fields[0]);
      int got;
      mwm_parts_t parts;

      mwm_partsInit(&parts);

      if (line[3] == 'E') {
        mwm_partsFromPattern(&parts, fields[1] != NULL ? fields[1] : "");
      } else {
        const char *pattern = fields[1] != NULL && fields[1][0] != 0 ? fields[1] : NULL;
        json_t args = fields[2] != NULL && fields[2][0] != 0
                          ? meta_toJSON(fields[2])
                          : (json_t){NULL, NULL, NULL, "", NULL, 0, NULL};

        mwm_partsForAct(&parts, pattern, args);
        args.release();
      }

      got = (int)(intptr_t)mwm_pinsMatch(pins, &parts) - 1;
      mwm_partsFree(&parts);
      ++messages;

      CHECK(got == want, "%s:%d (case %s) %s %s %s: got pin %d, microwizard %d",
            name, lineNo, caseName, line[3] == 'E' ? "actE" : "act",
            fields[1] != NULL ? fields[1] : "", fields[2] != NULL ? fields[2] : "",
            got, want);
    }
  }

  mwm_pinsFree(pins);

  CHECK(messages > 0, "%s: no messages", name);
  printf("pattern: %s - %d cases, %d messages, %d differ from microwizard\n", name,
         cases, messages, failures - before);
}

int main(int argc, char **argv) {

  patternTestJs();
  removing();
  throughService();
  numbers();

  if (argc > 1 && strcmp(argv[1], "-") == 0)
    trace(stdin, "stdin");
  else {

    char path[1024];
    FILE *in;

    snprintf(path, sizeof path, "%s/pattern-trace.txt",
             argc > 1 ? argv[1] : "test/fixtures");
    in = fopen(path, "r");

    if (in == NULL) {
      fprintf(stderr, "pattern: cannot open %s\n", path);
      return 1;
    }

    trace(in, "pattern-trace.txt");
    fclose(in);
  }

  if (failures != 0) {
    fprintf(stderr, "pattern: %d of %d checks failed\n", failures, checked);
    return 1;
  }

  printf("pattern: %d checks ok\n", checked);
  return 0;
}
