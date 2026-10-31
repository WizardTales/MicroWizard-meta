/**
 * balance.js's consume, as microwizard 1.6.2 runs it, without a clock or a
 * socket: what to do after each step of a call. client.c drives it - chooses
 * with visigoth, calls, sleeps - and asks this what comes next.
 *
 * Plain C, read by meta and by the C compiler. The JS, in short:
 *
 *   tryCall:
 *     choose fails                    -> done {err: 'no-current-target', msg}
 *     call answers                    -> done with the answer
 *     call fails with details.message 'retry_later_err_overload', or with
 *     message 'timeout' (connect or answer):
 *       errored() - the target is OPEN for closingTimeout (1000 ms)
 *       ++trys < 10                   -> tryCall again in retryTimeout (100 ms),
 *                                        with a new id
 *       else                          -> done {err: 'all-targets-overloaded', msg}
 *     call fails any other way        -> done {err, msg}: no errored(), no retry
 *
 * With one target that means no ten tries: the first failure opens it, the
 * try 100 ms later finds nothing to choose, and it is no-current-target.
 *
 * `patient` is the change microwizard's fix/consume-wait makes, and the
 * default here (docs/protokoll.md, "Verteilung"); patient 0 is 1.6.2 exactly:
 *
 *   - when choose fails after a failed try, wait once closingTimeout +
 *     retryTimeout - long enough for an opened target to be half-open - and
 *     choose again before giving up with no-current-target
 *   - the whole call takes at most `budget` ms, one request timeout - the
 *     client's timeout unless set: a try is given no more than what is left
 *     of it, a try chosen with nothing left is all-targets-overloaded, and a
 *     wait that would end past it gives up instead - no-current-target for
 *     the one wait, all-targets-overloaded for a retry
 */
#ifndef MWM_BALANCE_H
#define MWM_BALANCE_H

typedef struct {
  long closingTimeout; /* 1000: balance.js's, handed to visigoth */
  long retryTimeout;   /* 100 */
  int tries;           /* 10 */
  int patient;         /* 1; 0: as 1.6.2 */
  long budget;         /* patient only: 5555, one request timeout */
} bal_options_t;

void bal_defaults(bal_options_t *into);

typedef enum {
  BAL_CHOOSE,  /* choose a target (now, or after `wait` ms) */
  BAL_DONE     /* the call is over: `outcome` says how */
} bal_step_t;

typedef enum {
  BAL_ANSWERED,          /* the target's answer, res or an error it threw */
  BAL_NO_CURRENT_TARGET, /* {err: 'no-current-target', msg} */
  BAL_ALL_OVERLOADED     /* {err: 'all-targets-overloaded', msg} */
} bal_outcome_t;

typedef enum {
  BAL_OK,        /* an answer with res */
  BAL_RETRYABLE, /* timeout, or an error with details.message overload */
  BAL_ERROR      /* any other error: handed to the caller as it is */
} bal_result_t;

typedef struct {
  bal_options_t options;
  long started;
  int trys;
  int waited;

  /** What the last step said. */
  bal_step_t step;
  long wait;
  bal_outcome_t outcome;
} bal_consume_t;

/** A call begins: the first step is to choose, at once. */
void bal_begin(bal_consume_t *self, const bal_options_t *options, long now);

/**
 * Choose failed. Returns what next: BAL_DONE with no-current-target, or -
 * patient only - BAL_CHOOSE after `wait`.
 */
bal_step_t bal_noTarget(bal_consume_t *self, long now);

/**
 * How long the try about to start may wait for its answer, given the
 * target's own timeout: that timeout, or for a patient call no more than is
 * left of the budget (0 when nothing is: the target that was chosen is not
 * called, and the call is all-targets-overloaded, as balance.js has it).
 */
long bal_tryTimeout(const bal_consume_t *self, long targetTimeout, long now);

/**
 * A try ended. The caller has already told visigoth about a retryable
 * failure (errored()); this says whether to choose again after `wait`, or
 * that it is over.
 */
bal_step_t bal_tried(bal_consume_t *self, bal_result_t result, long now);

/** Whether the next try is a retry, which gets an id of its own. */
int bal_isRetry(const bal_consume_t *self);

const char *bal_outcomeText(bal_outcome_t outcome);

#endif
