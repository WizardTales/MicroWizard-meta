/**
 * Which pin a message goes to: the matching of index.js in microwizard as of
 * fix/pattern-shadow (#matchPin, add, convertPin, convertData), the one
 * matcher behind both halves - service.c for the calls that come in, client.c
 * for the calls that go out, where our own handlers and the pins of other
 * services are in one tree as they are in Node.
 *
 * Plain C, read by meta and by the C compiler. docs/protokoll.md section 10
 * says it in prose:
 *
 *   - a message is a set of parts `key:value`: every primitive field on the
 *     top level of the args (a string as it is, true/false, a number as JS
 *     prints it; null, objects and arrays are not parts), or the pairs of a
 *     pattern string exactly as convertData's regex finds them
 *   - a pin takes a message that has every pair of it, a wildcard pair
 *     (`cmd:*`) any value of that key; parts the pin does not name are
 *     passed over
 *   - of the pins that take it the one with the most pairs wins, then the
 *     one with the most exact pairs, then the first in the order the tree is
 *     walked: keys in alphabetical order, at each key the exact branch before
 *     the wildcard one - which is the pin exact on the first key that tells
 *     them apart
 *   - pins with the same pairs are one node of the tree: the one added last
 *     holds it, and when it is removed the one added before it
 *
 * A tree of pins holds an opaque value per pin; matching answers the value
 * of the winner, or NULL.
 */
#ifndef MWM_PATTERN_H
#define MWM_PATTERN_H

#include <meta_json.h>
#include <meta_owns.h>

typedef struct mwm_pins_s mwm_pins_t;

/** The parts of one message; mwm_partsFree when done. */
typedef struct {
  char **items;
  int count;
  int room;
} mwm_parts_t;

/** An empty tree, or NULL without memory. */
mwm_pins_t *mwm_pinsNew(void);

/** The tree and its nodes; the values are the caller's. */
void mwm_pinsFree(mwm_pins_t *pins);

/**
 * Adds a pin such as "role:cdn,cmd:*" holding value (not NULL): its node now
 * holds this value, whatever it held before. A pin without a pair is the
 * root, which takes every message. 0 without memory.
 */
int mwm_pinsAdd(mwm_pins_t *pins, const char *pattern, void *value);

/**
 * Takes value from the pin's node, as index.js's #dropPin takes a remote pin
 * whose last target left: the node goes to what was added there last before
 * (a handler of ours on the same pin), or to nothing - and nodes left with
 * neither a pin nor a branch go. 0 when the pin is not in the tree.
 */
int mwm_pinsRemove(mwm_pins_t *pins, const char *pattern, void *value);

/**
 * The value of the pin the message goes to, or NULL. Sorts the parts and
 * drops duplicates, as act does before it matches.
 */
void *mwm_pinsMatch(const mwm_pins_t *pins, mwm_parts_t *parts);

void mwm_partsInit(mwm_parts_t *parts);
void mwm_partsFree(mwm_parts_t *parts);

/** One part, copied. 0 without memory. */
int mwm_partsAdd(mwm_parts_t *parts, const char *part);

/** convertData(pattern).c: each pair as the regex matched it. */
int mwm_partsFromPattern(mwm_parts_t *parts, const char *pattern);

/**
 * The primitive fields on the top level of an object as `key:${value}`;
 * anything that is not an object has none.
 */
int mwm_partsFromArgs(mwm_parts_t *parts, json_t args);

/**
 * act(pattern, args): the pattern's pairs and the primitive fields of args,
 * both - a key in both is two parts. pattern may be NULL (act(args)).
 */
int mwm_partsForAct(mwm_parts_t *parts, const char *pattern, json_t args);

/**
 * convertData(pattern).o: the pairs as an object of strings, the key as the
 * regex took it, a later pair of the same key over an earlier one - what
 * act lays the args over.
 */
META_OWNS json_t mwm_patternObject(const char *pattern);

/**
 * pattern(pin) of index.js: convertData(pin).c sorted and joined with
 * commas - the key balance.js keeps a pin's client under. Freed by the
 * caller.
 */
META_OWNS char *mwm_patternKey(const char *pattern);

/**
 * `${value}` for a primitive JSON value, freed by the caller - or NULL when
 * it is null, an object or an array (typeof 'object'), or without memory.
 * A number as Number.prototype.toString writes the double JSON.parse makes
 * of it.
 */
META_OWNS char *mwm_jsText(yyjson_mut_val *node);

#endif
