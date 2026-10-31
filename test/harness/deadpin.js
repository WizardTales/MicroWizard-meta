// A pin whose last member is gone leaves the meta client's match, as
// microwizard fix/dead-pin has it (test/dead-pin-mesh.test.js there): A
// (the Node echo) serves role:deadpin,cmd:ping, B (the meta echo)
// role:deadpin,cmd:*, mwm-node --client calls cmd:ping. A answers while it is there; killed hard, B answers once
// SWIM has A faulty - not no-current-target for good, as before; started
// again, A answers again.
//
//   node deadpin.js [path/to/mwm-node]
//
// B is a meta node because a Node node of microwizard 1.5.0 has the dead
// pin itself: A's pin, role:deadpin,cmd:ping, takes the node of B's own
// handler for it, and with A gone B answers no-current-target.
//
// All on 127.0.0.1: the Node base on MWM_BASE_PORT (38999), the meta nodes
// on fixed ports below it in 38xxx, A on ports of its own choosing.
import { spawn } from 'node:child_process';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const node = path.resolve(process.argv[2] || path.join(here, '../../build/mwm-node'));
const BASE_PORT = Number(process.env.MWM_BASE_PORT || 38999);
const TAG = process.env.MWM_TAG || 'mwmtest';
const verbose = !!process.env.MWM_VERBOSE;
const CLIENT = [BASE_PORT - 30, BASE_PORT - 31];
const B = [BASE_PORT - 32, BASE_PORT - 33];

for (const p of [...CLIENT, ...B]) {
  if (p < 38000 || p > 38999) throw new Error(`port ${p} is outside 38xxx`);
}

const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => !k.startsWith('wzdc_')));
const children = new Set();
process.on('exit', () => children.forEach((c) => c.kill('SIGKILL')));
const events = [];
const waiters = [];
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function track (label, child) {
  children.add(child);
  child.on('exit', () => children.delete(child));
  readline.createInterface({ input: child.stdout }).on('line', (line) => {
    let e;
    try {
      e = JSON.parse(line);
    } catch (err) {
      if (verbose) console.error(`[${label}] ${line}`);
      return;
    }
    e.from = label;
    events.push(e);
    if (verbose) console.error(`[${label}]`, JSON.stringify(e));
    for (const w of [...waiters]) {
      if (w.match(e)) {
        waiters.splice(waiters.indexOf(w), 1);
        w.resolve(e);
      }
    }
  });
  child.stderr.on('data', (d) => verbose && process.stderr.write(`[${label}!] ${d}`));
  return child;
}

function waitFor (what, match, ms) {
  const seen = events.find(match);
  if (seen) return Promise.resolve(seen);
  return new Promise((resolve, reject) => {
    const w = { match, resolve };
    waiters.push(w);
    setTimeout(() => {
      if (waiters.includes(w)) {
        waiters.splice(waiters.indexOf(w), 1);
        reject(new Error(`timed out after ${ms} ms waiting for: ${what}`));
      }
    }, ms);
  });
}

function echo (label, pin) {
  return track(label, spawn(process.execPath, [path.join(here, 'echo.js'), label], {
    env: { ...env, MWM_BASE_PORT: String(BASE_PORT), MWM_TAG: TAG, MWM_ECHO_PIN: pin },
    stdio: ['ignore', 'pipe', 'pipe']
  }));
}

let step = 0;
const ok = (what) => console.log(`ok ${++step} - ${what}`);
const check = (good, what, detail) => {
  if (!good) throw new Error(`${what}: ${JSON.stringify(detail)}`);
};

let client;
let tags = 0;
function ask (call, ms = 20000) {
  const tag = ++tags;
  client.stdin.write(JSON.stringify({ tag, ...call }) + '\n');
  return waitFor(`answer to ${JSON.stringify(call)}`,
    (e) => e.from === 'client' && e.tag === tag && (e.ev === 'result' || e.ev === 'error'), ms);
}

const who = (e) => e.ev === 'result'
  ? (e.result?.msg?.from ?? (e.result === undefined ? 'undefined' : JSON.stringify(e.result)))
  : (typeof e.error?.err === 'string' ? e.error.err : 'error');

const X = 'role:deadpin,cmd:ping';

// calls X until `want` answers; what answered on the way there
async function until (want, act = X, tries = 80) {
  const seen = [];
  for (let i = 0; i < tries; i++) {
    const answered = who(await ask({ act, args: { n: i } }));
    seen.push(answered);
    if (answered === want) return seen;
    await sleep(250);
  }
  throw new Error(`never reached ${want}: ${[...new Set(seen)].join(', ')}`);
}

async function main () {
  track('base', spawn(process.execPath, [path.join(here, 'base.js'), 'base'], {
    env: { ...env, MWM_BASE_PORT: String(BASE_PORT), MWM_TAG: TAG },
    stdio: ['ignore', 'pipe', 'pipe']
  }));
  await waitFor('base ready', (e) => e.from === 'base' && e.ev === 'ready', 5000);

  client = track('client', spawn(node, [
    '--bases', `127.0.0.1:${BASE_PORT}`, '--tag', TAG,
    '--port', String(CLIENT[0]), '--listen-port', String(CLIENT[1]),
    '--events', '--client'
  ], { env, stdio: ['pipe', 'pipe', 'pipe'] }));
  await waitFor('client joined', (e) => e.from === 'client' && e.ev === 'joined', 5000);

  let a = echo('A', X);
  track('B', spawn(node, [
    '--bases', `127.0.0.1:${BASE_PORT}`, '--tag', TAG,
    '--port', String(B[0]), '--listen-port', String(B[1]),
    '--pin', 'role:deadpin,cmd:*', '--echo', 'deadpin'
  ], { env, stdio: ['ignore', 'pipe', 'pipe'] }));
  await waitFor('client has A', (e) => e.from === 'client' && e.ev === 'target-add' && e.pin === X, 8000);
  await waitFor('client has B', (e) => e.from === 'client' && e.ev === 'target-add' && e.pin === 'role:deadpin,cmd:*', 8000);

  await until('A');
  {
    // only B has a pin for cmd:fail; its echo answers 409
    const e = await ask({ act: 'role:deadpin,cmd:fail', args: {} });
    check(e.ev === 'result' && e.result?.code === 409, 'cmd:fail reaches B', e);
  }
  ok('cmd:ping reaches A; cmd:fail, which A has no pin for, reaches B');

  a.kill('SIGKILL');
  const seen = await until('meta');
  for (let i = 0; i < 3; i++) {
    const e = await ask({ act: X, args: {} });
    check(who(e) === 'meta', 'A gone: B answers', e);
  }
  ok(`A killed: once its target is gone, cmd:ping falls through to B (on the way: ${[...new Set(seen)].join(', ')})`);

  {
    const e = await ask({ actE: X, data: {} });
    check(who(e) === 'meta', 'actE with A gone', e);
    ok('actE falls through to B as well');
  }

  a = echo('A', X);
  await until('A');
  ok('A started again: cmd:ping reaches A once more');
}

const stopAll = async () => {
  for (const c of children) c.kill('SIGTERM');
  await sleep(300);
  for (const c of children) c.kill('SIGKILL');
};

let failed = false;
try {
  await main();
  console.log(`1..${step}`);
} catch (e) {
  failed = true;
  console.log(`not ok ${step + 1} - ${e.message}`);
  if (!verbose) console.log('# MWM_VERBOSE=1 shows every line the nodes wrote');
}
await stopAll();
process.exit(failed ? 1 : 0);
