/**
 * The benchmark's one handler, the one every test of
 * WizardTales/microservice-benchmarks serves: {cmd: 'add'} answers
 * {res: msg.a + msg.b}.
 */
#ifndef MWM_BENCH_ADD_H
#define MWM_BENCH_ADD_H

#include "service.h"

mwm_answer_t bench_add(json_t args);

#endif
