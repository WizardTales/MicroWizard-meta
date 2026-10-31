/**
 * Which pin wins, as microwizard's own matcher says - the reference for
 * src/pattern.c. Sets of pins and messages, fixed ones and seeded random
 * ones, through a real Micro of microwizard fix/pattern-shadow (index.js:
 * add, act, actE); test/unit/pattern_test reads the trace and must pick the
 * same pin for every message.
 *
 *   node pattern-trace.js <microwizard checkout> <seed> <random cases>
 *
 * The checkout is the one with the fixed #matchPin (fix/pattern-shadow, or
 * later); its node_modules must be there. Nothing is listened on, nothing
 * is sent: a Micro that only adds and acts is a pin tree.
 *
 * The trace, one line each, fields by tab:
 *
 *   case <n>
 *   pin <pattern>                        added in this order, the i-th answers i
 *   act <winner>\t<pattern>\t<args>      act(pattern, args); an empty pattern
 *                                        is act(args), empty args undefined
 *   actE <winner>\t<pattern>             actE(pattern, {})
 *
 * <winner> is the index of the pin that answered, -1 for no-target.
 */
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const [dir, seedArg, countArg] = process.argv.slice(2);

if (!dir) {
  console.error('usage: node pattern-trace.js <microwizard> <seed> <cases>');
  process.exit(2);
}

const { default: Micro } = await import(
  pathToFileURL(path.resolve(dir, 'index.js')).href
);

/* mulberry32: the same seed, the same trace, on every machine */
let state = Number(seedArg || 1) >>> 0;
const random = () => {
  state = (state + 0x6d2b79f5) >>> 0;
  let t = state;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
};
const pick = (list) => list[Math.floor(random() * list.length)];
const chance = (p) => random() < p;

const quiet = async (fn) => {
  const log = console.log;
  console.log = () => {};
  try {
    return await fn();
  } finally {
    console.log = log;
  }
};

const out = [];

const run = async (n, pins, messages) => {
  const mc = new Micro();

  pins.forEach((pin, i) => mc.add(pin, async () => i));
  out.push(`case ${n}`);
  for (const pin of pins) out.push(`pin ${pin}`);

  for (const m of messages) {
    if (m.e) {
      const got = await quiet(() => mc.actE(m.pattern, {}));
      out.push(`actE ${got ?? -1}\t${m.pattern}`);
    } else {
      const args = m.args === undefined ? undefined : JSON.parse(m.args);
      const got = await quiet(() =>
        m.pattern === '' ? mc.act(args) : mc.act(m.pattern, args)
      );
      out.push(`act ${got ?? -1}\t${m.pattern}\t${m.args ?? ''}`);
    }
  }
};

/* ------------------------------------------------------- fixed cases */

const fixed = [
  [
    ['role:mwmobserver,cmd:ping', 'role:mwmtest,cmd:*'],
    ['role:mwmtest,cmd:ping', 'role:mwmtest,cmd:fail', 'role:mwmobserver,cmd:ping']
  ],
  [
    ['a:*,b:1', 'a:1,b:*', 'b:1,c:*', 'a:*,c:1'],
    ['a:1,b:1', 'a:1,b:1,c:1', 'a:x,b:1,c:1', 'b:1,c:1']
  ],
  [
    ['role:x', 'role:x,cmd:*', 'role:a,cmd:ping', 'role:a,cmd:*'],
    ['role:x', 'role:x,cmd:go', 'role:a,cmd:ping', 'role:a,cmd:pong']
  ],
  /* the same pairs twice: the one added last holds the node */
  [
    ['role:a,cmd:*', 'cmd:*,role:a', 'role:a,cmd:ping'],
    ['role:a,cmd:x', 'role:a,cmd:ping']
  ],
  /* blanks, dashes, a pair twice, a value with a colon */
  [
    ['cmd :ping,role:a', 'a:1,a:1', 'a:*,a:1', 'url:http://x,role:b', 'a-b:*', 'a1:2'],
    ['role:a,cmd:ping', 'a:1', 'a:2', 'url:http://x,role:b', 'a-b:q,a1:2', 'role:a']
  ]
];

let n = 0;

for (const [pins, patterns] of fixed) {
  const messages = [];
  for (const pattern of patterns) {
    messages.push({ pattern });
    messages.push({ e: true, pattern });
  }
  await run(n++, pins, messages);
}

/* args the pattern string cannot hold: numbers, booleans, null, objects */
await run(n++, ['n:1,cmd:*', 'n:1.5', 'b:true', 'big:12345678901234567000', 'z:0.1', 'e:1e+21', 's:1e-7', 'o:*'], [
  { pattern: '', args: '{"n":1,"cmd":"x"}' },
  { pattern: '', args: '{"n":1.0}' },
  { pattern: '', args: '{"n":1.5}' },
  { pattern: '', args: '{"b":true}' },
  { pattern: '', args: '{"big":12345678901234567890}' },
  { pattern: '', args: '{"z":0.1}' },
  { pattern: '', args: '{"e":1e21}' },
  { pattern: '', args: '{"s":0.0000001}' },
  { pattern: '', args: '{"o":null}' },
  { pattern: '', args: '{"o":{"a":1}}' },
  { pattern: '', args: '{"o":[1]}' },
  { pattern: '', args: '{"o":-0}' },
  { pattern: 'n:1', args: '{"cmd":"y"}' },
  { pattern: 'n:2', args: '{"n":1,"cmd":"y"}' }
]);

/* ------------------------------------------------------ random cases */

const keys = ['a', 'b', 'cmd', 'role', 'a-b', 'a1', 'x', 'B', 'k_2'];
const values = ['1', '2', 'ping', 'x', 'a b', 'u:v', '10', '-1', 'true', '1.5'];

const pairText = (k, v) => (chance(0.03) ? `${k} :${v}` : `${k}:${v}`);

const randomPin = () => {
  const pairs = [];
  const count = 1 + Math.floor(random() * 4);
  for (let i = 0; i < count; ++i) {
    const k = pick(keys);
    if (!chance(0.05) && pairs.some((p) => p.k === k)) continue;
    pairs.push({ k, v: chance(0.4) ? '*' : pick(values) });
  }
  return pairs;
};

const toPattern = (pairs) => pairs.map(({ k, v }) => pairText(k, v)).join(',');

/** A message near a pin: its pairs, some changed, some dropped, some more. */
const near = (pin) => {
  const fields = {};
  for (const { k, v } of pin) {
    if (chance(0.1)) continue;
    fields[k] = v === '*' || chance(0.15) ? pick(values) : v;
  }
  for (let i = Math.floor(random() * 3); i > 0; --i) fields[pick(keys)] = pick(values);
  return fields;
};

const asArgs = (fields) => {
  const o = {};
  for (const [k, v] of Object.entries(fields)) {
    if (chance(0.15) && /^-?\d+(\.\d+)?$/.test(v)) o[k] = Number(v);
    else if (chance(0.05) && v === 'true') o[k] = true;
    else o[k] = v;
  }
  if (chance(0.1)) o[pick(keys)] = pick([null, { a: 1 }, [1, 2]]);
  return JSON.stringify(o);
};

const count = Number(countArg || 300);

for (let c = 0; c < count; ++c) {
  const pins = [];
  const pinCount = 1 + Math.floor(random() * 7);
  for (let i = 0; i < pinCount; ++i) pins.push(randomPin());

  const messages = [];
  for (let i = 0; i < 8; ++i) {
    const fields = chance(0.85) ? near(pick(pins)) : near(randomPin());
    const entries = Object.entries(fields);
    const form = random();

    if (form < 0.35) {
      messages.push({ pattern: entries.map(([k, v]) => `${k}:${v}`).join(',') || 'zz:1' });
    } else if (form < 0.55) {
      messages.push({ e: true, pattern: entries.map(([k, v]) => `${k}:${v}`).join(',') });
    } else if (form < 0.8) {
      messages.push({ pattern: '', args: asArgs(fields) });
    } else {
      /* part in the pattern, part in the args, a key sometimes in both */
      const cut = Math.floor(random() * (entries.length + 1));
      const head = entries.slice(0, cut).map(([k, v]) => `${k}:${v}`).join(',');
      const tail = Object.fromEntries(entries.slice(chance(0.2) ? 0 : cut));
      messages.push({ pattern: head || 'zz:1', args: asArgs(tail) });
    }
  }

  await run(n++, pins.map(toPattern), messages);
}

process.stdout.write(out.join('\n') + '\n');
