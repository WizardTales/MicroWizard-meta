/**
 * service.c line by line: what goes back for what comes in, and which
 * handler a call reaches. test/harness/reference.js is the same against a
 * real Node caller; this is the place for the rules one at a time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static mwm_answer_t general(json_t args) {
  json_t res = {who: "general"};
  return (mwm_answer_t){0, res};
}

static mwm_answer_t specific(json_t args) {
  json_t res = {who: "specific"};
  return (mwm_answer_t){0, res};
}

static mwm_answer_t later(json_t args) {
  json_t res = {who: "later"};
  return (mwm_answer_t){0, res};
}

static mwm_answer_t echoing(json_t args) {
  json_t res = {echo: args};
  return (mwm_answer_t){0, res};
}

static mwm_answer_t failing(json_t args) {
  json_t error = {message: "no", code: "E_NO"};
  return (mwm_answer_t){1, error};
}

static mwm_answer_t nothing(json_t args) {

  json_t none = {NULL, NULL, NULL, "", NULL, 0, NULL};

  return (mwm_answer_t){0, none};
}

/** The answer to `in`, compared with `want` (NULL: nothing goes back). */
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

/** What mwm_refuse says to `in` - over the cap, no handler runs. */
static void refuses(const char *in, const char *want, int line) {

  char *got = mwm_refuse(in, strlen(in));

  ++checked;

  if ((want == NULL) != (got == NULL) ||
      (want != NULL && strcmp(want, got) != 0)) {
    ++failures;
    fprintf(stderr, "FAIL line %d: refused %s\n  got  %s\n  want %s\n", line, in,
            got != NULL ? got : "(nothing)", want != NULL ? want : "(nothing)");
  }

  free(got);
}

#define REFUSES(in, want) refuses(in, want, __LINE__)

int main(void) {

  mwm_add("role:t,cmd:*", general);
  mwm_add("role:t,cmd:x,extra:1", specific);
  mwm_add("role:t,cmd:echo", echoing);
  mwm_add("role:t,cmd:fail", failing);
  mwm_add("role:t,cmd:none", nothing);
  mwm_add("role:u,cmd:x", general);
  mwm_add("role:u,cmd:x", later);

  ANSWERS("{\"h\":1}", "{\"h\":2}");

  /* the wildcard, and the pattern with more pairs over it */
  ANSWERS("{\"id\":\"a1\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":\"x\"}}",
          "{\"id\":\"a1\",\"k\":\"res\",\"sync\":true,\"res\":{\"who\":\"general\"}}");
  ANSWERS("{\"id\":\"a2\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":\"x\",\"extra\":1}}",
          "{\"id\":\"a2\",\"k\":\"res\",\"sync\":true,\"res\":{\"who\":\"specific\"}}");

  /* a number field matches its digits; a string "1" does too, as in JS */
  ANSWERS("{\"id\":\"a3\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":\"x\",\"extra\":\"1\"}}",
          "{\"id\":\"a3\",\"k\":\"res\",\"sync\":true,\"res\":{\"who\":\"specific\"}}");

  /* null is not a field: no match on cmd, nobody answers - no res */
  ANSWERS("{\"id\":\"a4\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":null}}",
          "{\"id\":\"a4\",\"k\":\"res\",\"sync\":true}");

  /* nor is an object */
  ANSWERS("{\"id\":\"a5\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":{\"x\":1}}}",
          "{\"id\":\"a5\",\"k\":\"res\",\"sync\":true}");

  /* the same pattern twice: the one added last */
  ANSWERS("{\"id\":\"a6\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"u\",\"cmd\":\"x\"}}",
          "{\"id\":\"a6\",\"k\":\"res\",\"sync\":true,\"res\":{\"who\":\"later\"}}");

  /* the handler gets args whole, the pattern fields included */
  ANSWERS("{\"id\":\"a7\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":\"echo\",\"data\":{\"n\":[1,2]}}}",
          "{\"id\":\"a7\",\"k\":\"res\",\"sync\":true,\"res\":{\"echo\":{\"role\":\"t\",\"cmd\":\"echo\",\"data\":{\"n\":[1,2]}}}}");

  /* aE: matched on p, the handler gets d */
  ANSWERS("{\"id\":\"e1\",\"k\":\"aE\",\"sync\":true,\"p\":\"role:t,cmd:echo\",\"d\":{\"n\":1}}",
          "{\"id\":\"e1\",\"k\":\"res\",\"sync\":true,\"res\":{\"echo\":{\"n\":1}}}");

  /* thrown: the fields, name Error, and a stack */
  ANSWERS("{\"id\":\"f1\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":\"fail\"}}",
          "{\"id\":\"f1\",\"k\":\"res\",\"sync\":true,\"error\":{\"message\":\"no\",\"code\":\"E_NO\","
          "\"name\":\"Error\",\"stack\":\"Error: no\\n    at mwm-node (args)\"}}");

  /* a handler answering nothing: res undefined */
  ANSWERS("{\"id\":\"n1\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":\"none\"}}",
          "{\"id\":\"n1\",\"k\":\"res\",\"sync\":true}");

  /* the id goes back as it came, a number too */
  ANSWERS("{\"id\":7,\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":\"x\"}}",
          "{\"id\":7,\"k\":\"res\",\"sync\":true,\"res\":{\"who\":\"general\"}}");

  /* sync false and an unknown k: nothing goes back */
  ANSWERS("{\"id\":\"s1\",\"k\":\"a\",\"sync\":false,\"args\":{\"role\":\"t\",\"cmd\":\"x\"}}", NULL);
  ANSWERS("{\"id\":\"s2\",\"k\":\"zz\",\"sync\":true}", NULL);

  /* not JSON: nothing here; main.c ends the connection */
  ANSWERS("{nope", NULL);

  /**
   * over the cap: overload with details, which balance.js counts and
   * retries elsewhere - for a or aE alike, the id as it came; nothing for
   * sync false, an unknown k or not JSON; a health ping is still answered
   */
  REFUSES("{\"id\":\"c1\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":\"x\"}}",
          "{\"id\":\"c1\",\"k\":\"res\",\"sync\":true,\"error\":{\"message\":"
          "\"retry_later_err_overload\",\"name\":\"Error\",\"details\":{\"message\":"
          "\"retry_later_err_overload\"}}}");
  REFUSES("{\"id\":8,\"k\":\"aE\",\"sync\":true,\"p\":\"role:t,cmd:x\",\"d\":{}}",
          "{\"id\":8,\"k\":\"res\",\"sync\":true,\"error\":{\"message\":"
          "\"retry_later_err_overload\",\"name\":\"Error\",\"details\":{\"message\":"
          "\"retry_later_err_overload\"}}}");
  REFUSES("{\"id\":\"c2\",\"k\":\"a\",\"sync\":false,\"args\":{\"role\":\"t\",\"cmd\":\"x\"}}", NULL);
  REFUSES("{\"id\":\"c3\",\"k\":\"zz\",\"sync\":true}", NULL);
  REFUSES("{nope", NULL);
  REFUSES("{\"h\":1}", "{\"h\":2}");

  /* offline: overload for everything that wants an answer */
  mwm_goOffline();
  ANSWERS("{\"id\":\"o1\",\"k\":\"a\",\"sync\":true,\"args\":{\"role\":\"t\",\"cmd\":\"x\"}}",
          "{\"id\":\"o1\",\"k\":\"res\",\"sync\":true,\"error\":{\"message\":"
          "\"retry_later_err_overload\",\"name\":\"Error\"}}");
  ANSWERS("{\"h\":1}", "{\"h\":2}");

  mwm_clear();

  if (failures != 0) {
    printf("service: %d of %d checks failed\n", failures, checked);
    return 1;
  }

  printf("service: %d checks ok\n", checked);
  return 0;
}
