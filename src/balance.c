/**
 * balance.js consume, step by step. balance.h has the JS in prose; the
 * comments here say where the patient variant departs from it.
 */
#include "balance.h"

void bal_defaults(bal_options_t *into) {

  into->closingTimeout = 1000;
  into->retryTimeout = 100;
  into->tries = 10;
  into->patient = 1;
  into->budget = 5555;
}

void bal_begin(bal_consume_t *self, const bal_options_t *options, long now) {

  self->options = *options;
  self->started = now;
  self->trys = 0;
  self->waited = 0;
  self->step = BAL_CHOOSE;
  self->wait = 0;
  self->outcome = BAL_ANSWERED;
}

static bal_step_t done(bal_consume_t *self, bal_outcome_t outcome) {

  self->step = BAL_DONE;
  self->wait = 0;
  self->outcome = outcome;

  return BAL_DONE;
}

static bal_step_t chooseIn(bal_consume_t *self, long wait) {

  self->step = BAL_CHOOSE;
  self->wait = wait;

  return BAL_CHOOSE;
}

/** Patient only: whether a wait of `wait` from now still ends inside the budget. */
static int fits(const bal_consume_t *self, long wait, long now) {
  return now + wait - self->started <= self->options.budget;
}

bal_step_t bal_noTarget(bal_consume_t *self, long now) {

  /**
   * 1.6.2: done({err: 'no-current-target'}) at once. Patient: after a failed
   * try, the target it opened is half-open again once closingTimeout has
   * passed - visigoth compares with "more than", so retryTimeout on top -
   * and one more choice is worth that wait, once.
   */
  if (self->options.patient && self->trys > 0 && !self->waited) {

    long wait = self->options.closingTimeout + self->options.retryTimeout;

    if (fits(self, wait, now)) {
      self->waited = 1;
      return chooseIn(self, wait);
    }
  }

  return done(self, BAL_NO_CURRENT_TARGET);
}

long bal_tryTimeout(const bal_consume_t *self, long targetTimeout, long now) {

  long left;

  if (!self->options.patient)
    return targetTimeout;

  left = self->options.budget - (now - self->started);

  if (left <= 0)
    return 0;

  return targetTimeout < left ? targetTimeout : left;
}

bal_step_t bal_tried(bal_consume_t *self, bal_result_t result, long now) {

  if (result != BAL_RETRYABLE)
    return done(self, BAL_ANSWERED);

  if (++self->trys >= self->options.tries)
    return done(self, BAL_ALL_OVERLOADED);

  if (self->options.patient && !fits(self, self->options.retryTimeout, now))
    return done(self, BAL_ALL_OVERLOADED);

  return chooseIn(self, self->options.retryTimeout);
}

int bal_isRetry(const bal_consume_t *self) { return self->trys > 0; }

const char *bal_outcomeText(bal_outcome_t outcome) {

  switch (outcome) {
  case BAL_NO_CURRENT_TARGET:
    return "no-current-target";
  case BAL_ALL_OVERLOADED:
    return "all-targets-overloaded";
  default:
    return "answered";
  }
}
