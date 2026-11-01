/**
 * The node as a library: what mwm-node does, for a service that brings
 * handlers of its own.
 *
 *   int main(int argc, char **argv) {
 *
 *     mwm_add("role:cdn,cmd:get", get);      before the node starts
 *     mwm_add("role:cdn,cmd:put", put);
 *
 *     return mwm_nodeMain(argc, argv);       the command line of mwm-node
 *   }
 *
 * mwm_nodeMain reads the environment and the command line exactly as
 * mwm-node does (the head of src/main.c), joins the mesh, serves the pins
 * it is given with every handler added so far, and returns only when it
 * could not start - 1 or 2, with the reason on stderr. A node that started
 * ends the process itself, after SIGTERM or SIGINT, as mwm-node does.
 *
 * Plain C, read by meta and by the C compiler.
 */
#ifndef MWM_NODE_H
#define MWM_NODE_H

int mwm_nodeMain(int argc, char **argv);

#endif
