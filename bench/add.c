/** bench_add: {res: a + b}, as suites/local.js and mw.js have it. */
#include "add.h"

mwm_answer_t bench_add(json_t args) {

  long a = args.a, b = args.b;
  long sum = a + b;
  json_t res = {res: sum};

  return (mwm_answer_t){0, res};
}
