// Drives the real @wzrdtales/visigoth 2.0.5 through a sequence of adds,
// removes, choices, failures and clock steps, and writes the sequence and
// what visigoth decided, so test/unit/visigoth_test.c can replay the same
// sequence through the meta port and compare line by line.
//
//   node visigoth-trace.js [seed] [steps] > ../fixtures/visigoth-trace.txt
//
// Each line: <op> [arg] TAB <result> TAB <ring from the top, name:STATUS ...>
//
//   add <name>       result: ok | full
//   remove <name>    result: ok
//   choose           result: <name> | none
//   fail <name>      the failure handler of that target (balance.js's errored())
//   time <ms>        the clock moves on by ms
//
// The clock is Date.now, replaced: visigoth reads nothing else. The
// closingTimeout is microwizard's 1000 and the ring is shrunk to 6 so "ring
// full" happens too.
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const visigoth = require('@wzrdtales/visigoth');

let clock = 1_000_000;
Date.now = () => clock;

let seed = Number(process.argv[2] || 7);
const steps = Number(process.argv[3] || 3000);
const random = (n) => {
  seed = (seed * 1103515245 + 12345) % 2147483648;
  return seed % n;
};

const vg = visigoth({ closingTimeout: 1000, ringSize: 6 });
const nodes = new Map(); // name -> e
let next = 0;

const ring = () =>
  vg.upstreams$.length === 0
    ? '-'
    : vg.upstreams$
        .getAll()
        .map((e) => `${e.u.target}:${e.u.meta$.status}`)
        .join(' ');

const out = [];
const emit = (op, result) => out.push(`${op}\t${result}\t${ring()}`);

for (let i = 0; i < steps; ++i) {
  const r = random(100);
  const names = [...nodes.keys()];

  if (r < 8 || names.length === 0) {
    const name = `t${next++}`;
    const res = vg.add(name);
    if (res.code === 0) nodes.set(name, res.e);
    emit(`add ${name}`, res.code === 0 ? 'ok' : 'full');
  } else if (r < 13) {
    const name = names[random(names.length)];
    vg.remove(nodes.get(name));
    nodes.delete(name);
    emit(`remove ${name}`, 'ok');
  } else if (r < 60) {
    let chosen = 'none';
    vg.choose((err, target, errored) => {
      if (!err) chosen = target;
    });
    emit('choose', chosen);
  } else if (r < 80) {
    const name = names[random(names.length)];
    // the handler visigoth would have handed out with a choice of this target
    vg.failureStrategy$(nodes.get(name).u)();
    emit(`fail ${name}`, 'ok');
  } else {
    const ms = random(4) === 0 ? 1000 + random(3) - 1 : random(700);
    clock += ms;
    emit(`time ${ms}`, 'ok');
  }
}

process.stdout.write(out.join('\n') + '\n');
