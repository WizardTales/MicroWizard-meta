/**
 * bench-node: mwm-node as it is - src/main.c, its listens, its serving
 * tasks - with the benchmark's handler added: cmd:add answers {res: a + b}.
 * Started as a lone base on 127.0.0.1, so no datagram leaves it:
 *
 *   bench-node --isbase --port 38990 --tag mwmbench --pin cmd:add \
 *              --listen-port 38101
 *
 * The Makefile puts src/main.c in front of this, its main renamed
 * mwm_node_main, so a change to how mwm-node serves (a task per call instead
 * of workers per connection) is what is measured, not a copy of it.
 */
#include "add.h"

int main(int argc, char **argv) {

  /* a + b never parks: answered by the connection's reader, no task */
  mwm_addInline("cmd:add", bench_add);

  return mwm_node_main(argc, argv);
}
