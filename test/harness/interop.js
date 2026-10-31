// The meta node in a mesh of Node nodes, all on 127.0.0.1.
//
//   node interop.js [path/to/mwm-node]
//
// 1. base, echo service and a watching client come up (Node, microwizard
//    1.5.0); a second Node client calls the echo service - the reference
//    every later step is measured against
// 2. mwm-node joins: the Node base and the watcher report it with its pin,
//    and it reports the Node members, the echo service's TCP address included
// 3. mwm-node is killed hard: both report it gone
// 4. mwm-node comes back on the same SWIM port: both report it again, with a
//    new identifier and a higher incarnation
// 5. the echo service is killed hard: mwm-node reports it gone
//
// Exits non-zero at the first step that does not happen in time.
import { spawn } from 'node:child_process';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const node = path.resolve(process.argv[2] || path.join(here, '../../build/mwm-node'));
const BASE_PORT = Number(process.env.MWM_BASE_PORT || 38999);
const META_SWIM = BASE_PORT - 8;
const META_TCP = BASE_PORT - 9;
const TAG = process.env.MWM_TAG || 'mwmtest';
const PIN = 'role:meta,cmd:*';
const verbose = !!process.env.MWM_VERBOSE;

const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => !k.startsWith('wzdc_')));
const children = new Set();
process.on('exit', () => children.forEach((c) => c.kill('SIGKILL')));
const events = [];
const waiters = [];
let failed = false;

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

function nodeScript (script, label, ...args) {
  return track(label, spawn(process.execPath, [path.join(here, script), label, ...args], {
    env: { ...env, MWM_BASE_PORT: String(BASE_PORT), MWM_TAG: TAG },
    stdio: ['ignore', 'pipe', 'pipe']
  }));
}

function metaNode (label) {
  return track(label, spawn(node, [
    '--bases', `127.0.0.1:${BASE_PORT}`,
    '--tag', TAG,
    '--pin', PIN,
    '--listen-port', String(META_TCP),
    '--port', String(META_SWIM),
    '--events'
  ], { env, stdio: ['ignore', 'pipe', 'pipe'] }));
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

let step = 0;
function ok (what) {
  console.log(`ok ${++step} - ${what}`);
}

function since (t) {
  return (e) => e.t >= t;
}

async function main () {
  // 1 ------------------------------------------------------------------
  nodeScript('base.js', 'base');
  await waitFor('base ready', (e) => e.from === 'base' && e.ev === 'ready', 5000);
  const echo = nodeScript('echo.js', 'echo');
  await waitFor('echo ready', (e) => e.from === 'echo' && e.ev === 'ready', 5000);
  nodeScript('client.js', 'watcher', '-', '{}', '--watch');
  await waitFor('watcher ready', (e) => e.from === 'watcher' && e.ev === 'ready', 5000);
  const echoSeen = await waitFor('watcher sees echo',
    (e) => e.from === 'watcher' && e.ev === 'add' && e.pin === 'role:echo,cmd:*', 5000);

  nodeScript('client.js', 'caller', 'role:echo,cmd:ping', '{"n":1,"s":"hello"}');
  const answer = await waitFor('caller gets an answer',
    (e) => e.from === 'caller' && (e.ev === 'result' || e.ev === 'error'), 15000);
  if (answer.ev !== 'result' || answer.result.code !== 200 || answer.result.msg.echo.s !== 'hello') {
    throw new Error(`Node to Node call went wrong: ${JSON.stringify(answer)}`);
  }
  ok('Node client calls the Node echo service through the mesh (the reference)');

  // 2 ------------------------------------------------------------------
  let t = Date.now();
  metaNode('meta');
  const started = await waitFor('meta started', (e) => e.from === 'meta' && e.ev === 'started', 3000);
  await waitFor('meta joined', (e) => e.from === 'meta' && e.ev === 'joined', 5000);
  ok('mwm-node joins the Node base');

  const wAdd = await waitFor('watcher sees mwm-node with its pin',
    (e) => e.from === 'watcher' && e.ev === 'add' && e.pin === PIN && since(t)(e), 6000);
  const bAdd = await waitFor('base sees mwm-node with its pin',
    (e) => e.from === 'base' && e.ev === 'add' && e.pin === PIN && since(t)(e), 6000);
  if (wAdd.port !== META_TCP || wAdd.host !== '127.0.0.1') throw new Error(`announced TCP address is off: ${JSON.stringify(wAdd)}`);
  if (!wAdd.id.endsWith(`~${started.id}`)) throw new Error(`the client id does not carry our identifier$: ${wAdd.id}`);
  ok(`Node watcher and Node base see mwm-node alive with pin ${PIN} at 127.0.0.1:${META_TCP}`);

  const mEcho = await waitFor('mwm-node sees echo',
    (e) => e.from === 'meta' && e.ev === 'add' && e.pin === 'role:echo,cmd:*', 6000);
  await waitFor('mwm-node sees the base',
    (e) => e.from === 'meta' && e.ev === 'add' && e.pin === 'role:mesh,base:true', 6000);
  await waitFor('mwm-node sees the watcher',
    (e) => e.from === 'meta' && e.ev === 'add' && e.pin === 'null:true', 6000);
  if (mEcho.tcp !== `${echoSeen.host}:${echoSeen.port}`) {
    throw new Error(`mwm-node has echo at ${mEcho.tcp}, the watcher at ${echoSeen.host}:${echoSeen.port}`);
  }
  ok(`mwm-node sees the Node members, echo at ${mEcho.tcp} as the watcher has it`);

  // 3 ------------------------------------------------------------------
  const first = [...children].find((c) => c.spawnfile === node);
  t = Date.now();
  first.kill('SIGKILL');
  const wGone = await waitFor('watcher drops mwm-node',
    (e) => e.from === 'watcher' && e.ev === 'remove' && e.pin === PIN && since(t)(e), 10000);
  await waitFor('base drops mwm-node',
    (e) => e.from === 'base' && e.ev === 'remove' && e.pin === PIN && since(t)(e), 10000);
  ok(`killed mwm-node is faulty for the Node side after ${wGone.t - t} ms`);

  // 4 ------------------------------------------------------------------
  await sleep(300);
  t = Date.now();
  metaNode('meta2');
  const again = await waitFor('meta2 started', (e) => e.from === 'meta2' && e.ev === 'started', 3000);
  const wBack = await waitFor('watcher sees mwm-node again',
    (e) => e.from === 'watcher' && e.ev === 'add' && e.pin === PIN && since(t)(e), 6000);
  await waitFor('base sees mwm-node again',
    (e) => e.from === 'base' && e.ev === 'add' && e.pin === PIN && since(t)(e), 6000);
  if (!wBack.id.endsWith(`~${again.id}`) || again.id === started.id) {
    throw new Error(`restart should come back with a new identifier: ${wBack.id}`);
  }
  ok(`mwm-node restarted on the same SWIM port is alive again (${wBack.t - t} ms), new identifier`);

  // 5 ------------------------------------------------------------------
  await waitFor('meta2 sees echo', (e) => e.from === 'meta2' && e.ev === 'add' && e.pin === 'role:echo,cmd:*', 6000);
  t = Date.now();
  echo.kill('SIGKILL');
  const mGone = await waitFor('mwm-node drops echo',
    (e) => e.from === 'meta2' && e.ev === 'remove' && e.pin === 'role:echo,cmd:*' && since(t)(e), 10000);
  await waitFor('watcher drops echo',
    (e) => e.from === 'watcher' && e.ev === 'remove' && e.pin === 'role:echo,cmd:*' && since(t)(e), 10000);
  ok(`killed Node echo is faulty for mwm-node after ${mGone.t - t} ms`);
}

const stopAll = async () => {
  for (const c of children) c.kill('SIGTERM');
  await sleep(300);
  for (const c of children) c.kill('SIGKILL');
};

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
