/**
 * bench-node: mwm-node as it is - src/main.c, its listens, its serving
 * tasks - with the benchmark's handler added: cmd:add answers {res: a + b}.
 * Started as a lone base on 127.0.0.1, so no datagram leaves it:
 *
 *   bench-node --isbase --port 38990 --tag mwmbench --pin cmd:add \
 *              --listen-port 38101
 *
 * The node is src/main.c's, called as mwm_nodeMain (node.h), so a change
 * to how mwm-node serves (a task per call instead of workers per
 * connection) is what is measured, not a copy of it.
 */
#include "add.h"
#include "node.h"

int main(int argc, char **argv) {

  /* a + b never parks: answered by the connection's reader, no task */
  mwm_addInline("cmd:add", bench_add);

  return mwm_nodeMain(argc, argv);
}
