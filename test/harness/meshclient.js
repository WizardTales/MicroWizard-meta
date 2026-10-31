// The meta client in a mesh of Node nodes, all on 127.0.0.1: mwm-node
// --client calls services the way a Node service calls them, and has to get
// what a Node caller gets.
//
//   node meshclient.js [path/to/mwm-node]
//
// 1. a Node base, the Node echo service, the meta echo service and a meta
//    client come up; the client makes targets of both services' pins
// 2. case by case (answer, application error, thrown error, overload with
//    one target, no handler, past the timeout, actE) the meta client calls
//    the Node echo and the meta echo, and a Node client calls the Node echo:
//    all three have to settle the same way
// 3. a second Node echo: calls go round robin; both overloaded is
//    no-current-target, for the Node client as for the meta one
// 4. the second Node echo is killed hard: every call still answers, from
//    the first one - the dead one costs a connect timeout and a retry - and
//    the client drops the target once SWIM has it faulty
// 5. a meta echo is killed and started again at once on the same ports: the
//    client drops the old target and takes the new one before anybody could
//    have found the old one faulty (sneeze 0.13.1's rule)
// 6. a Node service with two listens and a meta node with two pins: Node
//    sees the same shape from both - two members, one instance, a pin each,
//    the same config keys - and the meta client calls the second pin of each
//
// Exits non-zero at the first step that does not happen.
import { spawn } from 'node:child_process';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const node = path.resolve(process.argv[2] || path.join(here, '../../build/mwm-node'));
const BASE_PORT = Number(process.env.MWM_BASE_PORT || 38999);
const TAG = process.env.MWM_TAG || 'mwmtest';
const verbose = !!process.env.MWM_VERBOSE;

// fixed SWIM and TCP ports for the meta nodes, all below the base in 38xxx
const PORTS = {
  metaEcho: [BASE_PORT - 18, BASE_PORT - 19],
  client: [BASE_PORT - 17, BASE_PORT - 20],
  restart: [BASE_PORT - 16, BASE_PORT - 15],
  multi: [BASE_PORT - 14, BASE_PORT - 12] // two pins: +0 and +1 each
};

for (const [swim, tcp] of Object.values(PORTS)) {
  for (const p of [swim, tcp, swim + 1, tcp + 1]) {
    if (p < 38000 || p > 38999) throw new Error(`port ${p} is outside 38xxx`);
  }
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
    e.at = Date.now();
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

function nodeScript (script, label, extraEnv = {}, ...args) {
  return track(label, spawn(process.execPath, [path.join(here, script), label, ...args], {
    env: { ...env, MWM_BASE_PORT: String(BASE_PORT), MWM_TAG: TAG, ...extraEnv },
    stdio: ['ignore', 'pipe', 'pipe']
  }));
}

function metaNode (label, [swim, tcp], ...args) {
  return track(label, spawn(node, [
    '--bases', `127.0.0.1:${BASE_PORT}`,
    '--tag', TAG,
    '--port', String(swim),
    '--listen-port', String(tcp),
    '--events',
    ...args
  ], { env, stdio: ['pipe', 'pipe', 'pipe'] }));
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

function check (good, what, detail) {
  if (!good) throw new Error(`${what}: ${typeof detail === 'string' ? detail : JSON.stringify(detail)}`);
}

// the meta client: one JSON line per call on stdin, the outcome by tag
let client;
let tags = 0;
function ask (call, ms = 20000) {
  const tag = ++tags;
  client.stdin.write(JSON.stringify({ tag, ...call }) + '\n');
  return waitFor(`answer to ${JSON.stringify(call)}`,
    (e) => e.from === 'client' && e.tag === tag && (e.ev === 'result' || e.ev === 'error'), ms);
}

// a Node caller, as reference.js has it: what actAsync settles with
function nodeCall (pattern, args = '{}') {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [path.join(here, 'client.js'), 'ref', pattern, args], {
      env: { ...env, MWM_BASE_PORT: String(BASE_PORT), MWM_TAG: TAG, MWM_CLIENT_WAIT: '10000' },
      stdio: ['ignore', 'pipe', 'ignore']
    });
    children.add(child);
    let out = '';
    child.stdout.on('data', (d) => { out += d; });
    child.on('exit', () => {
      children.delete(child);
      resolve(out.split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l))
        .find((e) => e.ev === 'result' || e.ev === 'error'));
    });
  });
}

// one shape for the meta client's outcome and the Node client's
function settled (e) {
  if (!e) return 'nothing';
  if (e.ev === 'result') return { result: e.result === undefined ? 'undefined' : e.result };
  const err = e.error.err;
  return {
    rejected: typeof err === 'string' ? err : { message: err.message, code: err.code }
  };
}

// what may differ between the services: the role and who answered
const alike = (a, b) => JSON.stringify(a, (k, v) =>
  typeof v === 'string' ? v.replace(/^(echo|meta|echo2)$/, 'X') : v) ===
  JSON.stringify(b, (k, v) => typeof v === 'string' ? v.replace(/^(echo|meta|echo2)$/, 'X') : v);

async function main () {
  // 1 ------------------------------------------------------------------
  nodeScript('base.js', 'base');
  await waitFor('base ready', (e) => e.from === 'base' && e.ev === 'ready', 5000);
  nodeScript('echo.js', 'echo');
  await waitFor('echo ready', (e) => e.from === 'echo' && e.ev === 'ready', 5000);
  metaNode('meta', PORTS.metaEcho, '--pin', 'role:meta,cmd:*', '--echo', 'meta');
  await waitFor('meta joined', (e) => e.from === 'meta' && e.ev === 'joined', 5000);
  // the Node callers here are microwizard as core pins it, so the meta
  // client is held to consume as it is there: --no-patient
  client = metaNode('client', PORTS.client, '--client', '--no-patient');
  await waitFor('client joined', (e) => e.from === 'client' && e.ev === 'joined', 5000);

  const tEcho = await waitFor('client has the Node echo as a target',
    (e) => e.from === 'client' && e.ev === 'target-add' && e.pin === 'role:echo,cmd:*', 6000);
  const tMeta = await waitFor('client has the meta echo as a target',
    (e) => e.from === 'client' && e.ev === 'target-add' && e.pin === 'role:meta,cmd:*', 6000);
  check(/^connectTimeout:355,host:127\.0\.0\.1,ismesh:true,model:consume,pin:role:echo,cmd:\*,port:\d+,timeout:5555,type:tcp~[0-9a-f]{11}~role:echo,cmd:\*~\d+$/.test(tEcho.id),
    'target id is not mesh.js\'s', tEcho.id);
  check(tMeta.tcp === `127.0.0.1:${PORTS.metaEcho[1]}`, 'meta echo target address', tMeta);
  ok(`the meta client makes targets of both services, keyed as mesh.js keys them (${tEcho.id.replace(/~.*/, '~…')})`);

  // 2 ------------------------------------------------------------------
  const cases = [
    ['an answer', 'ping', { n: 1, s: 'hello' },
      (s) => s.result && s.result.code === 200 && s.result.msg.echo.n === 1 && s.result.msg.echo.s === 'hello'],
    ['an application error is an answer', 'fail', {},
      (s) => s.result && s.result.code === 409 && s.result.err === 'conflict on purpose'],
    ['a thrown error rejects with {err, msg}', 'throw', {},
      (s) => s.rejected && s.rejected.message === 'thrown on purpose' && s.rejected.code === 'E_PURPOSE'],
    ['overload with one target: no-current-target', 'overload', {},
      (s) => s.rejected === 'no-current-target'],
    ['no handler behind the wildcard: undefined', 'nothing', {},
      (s) => s.result === 'undefined'],
    ['past the timeout with one target: no-current-target', 'slow', { ms: 7000 },
      (s) => s.rejected === 'no-current-target']
  ];

  // the Node callers are processes of their own and run side by side; the
  // meta client's calls share one visigoth per pin, as a Node process's do,
  // so they go one after the other, with time for an opened target to be
  // half-open again after the cases that open one
  const nodeRuns = cases.map(([, cmd, args]) => nodeCall(`role:echo,cmd:${cmd}`, JSON.stringify(args)));
  const runs = [];
  for (const [i, [what, cmd, args, good]] of cases.entries()) {
    const [mEcho, mMeta] = await Promise.all([
      ask({ act: `role:echo,cmd:${cmd}`, args }),
      ask({ act: `role:meta,cmd:${cmd}`, args })
    ]);
    if (cmd === 'overload' || cmd === 'slow') await sleep(1200);
    runs.push({ what, good, mEcho, mMeta, nEcho: await nodeRuns[i] });
  }

  for (const { what, good, mEcho, mMeta, nEcho } of runs) {
    const a = settled(mEcho);
    const b = settled(mMeta);
    const c = settled(nEcho);
    check(good(a) && good(b) && good(c) && alike(a, c) && alike(b, c), what,
      `\n  meta→echo ${JSON.stringify(a)}\n  meta→meta ${JSON.stringify(b)}\n  node→echo ${JSON.stringify(c)}`);
    ok(`meta client and Node client alike: ${what}`);
  }

  {
    const slow = runs[5];
    check(slow.mEcho.ms >= 5555 && slow.mEcho.ms < 6500, 'the timeout is the announced 5555 ms, and the retry 100 ms', slow.mEcho);
  }

  {
    const [e, m] = await Promise.all([
      ask({ actE: 'role:echo,cmd:ping', data: { x: 1 } }),
      ask({ actE: 'role:meta,cmd:ping', data: { x: 1 } })
    ]);
    check(e.ev === 'result' && e.result.msg.echo.x === 1 && m.ev === 'result' && m.result.msg.echo.x === 1,
      'actE', { e, m });
    ok('actE reaches both services, matched on the pattern, with the data as d');
  }

  {
    const e = await ask({ act: 'role:nobody,cmd:x', args: {} });
    check(e.ev === 'result' && e.result === undefined, 'no pin at all', e);
    ok('a pattern nobody serves resolves undefined, as in Node');
  }

  // 3 ------------------------------------------------------------------
  const echo2 = nodeScript('echo.js', 'echo2');
  await waitFor('echo2 ready', (e) => e.from === 'echo2' && e.ev === 'ready', 5000);
  await waitFor('client has echo2 as a target',
    (e) => e.from === 'client' && e.ev === 'target-add' && e.pin === 'role:echo,cmd:*' && e.id !== tEcho.id, 6000);
  await sleep(1200); // whatever was opened before is half-open again

  {
    const froms = [];
    for (let i = 0; i < 4; ++i) {
      const e = await ask({ act: 'role:echo,cmd:ping', args: { i } });
      check(e.ev === 'result', 'round robin', e);
      froms.push(e.result.msg.from);
    }
    check(froms.join() === 'echo,echo2,echo,echo2' || froms.join() === 'echo2,echo,echo2,echo',
      'round robin over two targets', froms);
    ok(`two targets are called round robin (${froms.join(', ')})`);
  }

  {
    const [m, n] = await Promise.all([
      ask({ act: 'role:echo,cmd:overload', args: {} }),
      nodeCall('role:echo,cmd:overload')
    ]);
    check(settled(m).rejected === 'no-current-target' && alike(settled(m), settled(n)) && m.ms >= 100 && m.ms < 600,
      'both overloaded', { m, n });
    ok(`two targets, both overloaded: one retry, then no-current-target after ${m.ms} ms, as from Node`);
  }

  // 4 ------------------------------------------------------------------
  await sleep(1200);
  let t = Date.now();
  echo2.kill('SIGKILL');
  // a call already on its way to echo2 when it dies waits out its 5555 ms,
  // as in Node: the connection's end fails nothing that was sent on it
  await sleep(300);
  {
    const outcomes = [];
    for (let i = 0; i < 6; ++i) outcomes.push(await ask({ act: 'role:echo,cmd:ping', args: { i } }));
    check(outcomes.every((e) => e.ev === 'result' && e.result.msg.from === 'echo'),
      'failover', outcomes.map((e) => [e.ev, e.ms, e.result && e.result.msg.from]));
    const slowest = Math.max(...outcomes.map((e) => e.ms));
    check(slowest >= 355 && slowest < 1500, 'the dead target costs a connect timeout and a retry', outcomes.map((e) => e.ms));
    ok(`echo2 killed: every call answered by echo, the slowest after ${slowest} ms (connect timeout, retry)`);
  }
  const gone = await waitFor('client drops echo2',
    (e) => e.from === 'client' && e.ev === 'target-remove' && e.pin === 'role:echo,cmd:*' && e.at >= t, 10000);
  ok(`the client drops the killed target once SWIM has it faulty (${gone.at - t} ms)`);

  // 5 ------------------------------------------------------------------
  metaNode('r1', PORTS.restart, '--pin', 'role:again,cmd:*', '--echo', 'again');
  const r1 = await waitFor('r1 started', (e) => e.from === 'r1' && e.ev === 'started', 3000);
  const old = await waitFor('client has r1',
    (e) => e.from === 'client' && e.ev === 'target-add' && e.pin === 'role:again,cmd:*', 6000);
  check(old.id.endsWith(`~${r1.id}`), 'r1 target', old);
  {
    const e = await ask({ act: 'role:again,cmd:ping', args: {} });
    check(e.ev === 'result', 'r1 answers', e);
  }

  t = Date.now();
  [...children].find((c) => c.spawnargs.includes('role:again,cmd:*')).kill('SIGKILL');
  await sleep(50);
  metaNode('r2', PORTS.restart, '--pin', 'role:again,cmd:*', '--echo', 'again');
  const r2 = await waitFor('r2 started', (e) => e.from === 'r2' && e.ev === 'started', 3000);
  const dropped = await waitFor('client drops r1',
    (e) => e.from === 'client' && e.ev === 'target-remove' && e.id === old.id, 6000);
  const fresh = await waitFor('client has r2',
    (e) => e.from === 'client' && e.ev === 'target-add' && e.pin === 'role:again,cmd:*' && e.id.endsWith(`~${r2.id}`), 6000);
  check(dropped.at - t < 1500, 'the old target goes before it could be found faulty', { ms: dropped.at - t });
  check(events.indexOf(dropped) < events.indexOf(fresh), 'the old one goes before the new one comes', { dropped, fresh });
  {
    const e = await ask({ act: 'role:again,cmd:ping', args: {} });
    check(e.ev === 'result' && e.ms < 300, 'the new one answers at once', e);
  }
  ok(`restarted on the same ports: the old target dropped after ${dropped.at - t} ms, before the new one was added, and the new one answers`);

  // 6 ------------------------------------------------------------------
  nodeScript('client.js', 'watcher', {}, '-', '{}', '--watch');
  await waitFor('watcher ready', (e) => e.from === 'watcher' && e.ev === 'ready', 5000);
  t = Date.now();
  nodeScript('echo.js', 'two', { MWM_ECHO_PINS: 'role:twoA,cmd:*;role:twoB,cmd:*' });
  metaNode('mtwo', PORTS.multi, '--pin', 'role:mtwoA,cmd:*', '--pin', 'role:mtwoB,cmd:*',
    '--echo', 'mtwoA', '--echo', 'mtwoB');

  const seen = {};
  for (const pin of ['role:twoA,cmd:*', 'role:twoB,cmd:*', 'role:mtwoA,cmd:*', 'role:mtwoB,cmd:*']) {
    seen[pin] = await waitFor(`watcher sees ${pin}`,
      (e) => e.from === 'watcher' && e.ev === 'add' && e.pin === pin && e.t >= t, 8000);
  }

  const shape = (a, b) => {
    const idA = a.id.split('~');
    const idB = b.id.split('~');
    // utilPattern(config) ~ instance ~ pin ~ Date.now()
    return {
      instanceShared: idA[1] === idB[1] && /^[0-9a-f]{11}$/.test(idA[1]),
      pinInId: idA[2] === a.pin && idB[2] === b.pin,
      ownPorts: a.port !== b.port,
      keys: JSON.stringify(a.keys) === JSON.stringify(b.keys) ? a.keys.join(',') : 'differ',
      parts: idA.length === 4 && idB.length === 4
    };
  };
  const nodeShape = shape(seen['role:twoA,cmd:*'], seen['role:twoB,cmd:*']);
  const metaShape = shape(seen['role:mtwoA,cmd:*'], seen['role:mtwoB,cmd:*']);
  check(nodeShape.instanceShared && nodeShape.pinInId && nodeShape.ownPorts && nodeShape.parts,
    'the Node service with two listens is not what the harness expects', nodeShape);
  check(JSON.stringify(nodeShape) === JSON.stringify(metaShape),
    'Node sees the two pins of mwm-node not as it sees a Node service with two listens',
    `\n  node ${JSON.stringify(nodeShape)}\n  meta ${JSON.stringify(metaShape)}`);
  {
    const swims = events.filter((e) => e.from === 'mtwo' && e.ev === 'started');
    check(swims.length === 2 && swims[0].swim !== swims[1].swim && swims[0].id !== swims[1].id &&
      swims[0].instance === swims[1].instance, 'mwm-node: one SWIM member per pin', swims);
  }
  ok('two pins in mwm-node are two members to Node, exactly as two listens of a Node service are: one instance, a pin, a port and an identifier$ each');

  for (const pin of ['role:twoB,cmd:*', 'role:mtwoB,cmd:*']) {
    await waitFor(`client has ${pin}`, (e) => e.from === 'client' && e.ev === 'target-add' && e.pin === pin, 6000);
  }
  {
    const [a, b, c] = await Promise.all([
      ask({ act: 'role:twoB,cmd:ping', args: { k: 1 } }),
      ask({ act: 'role:mtwoB,cmd:ping', args: { k: 2 } }),
      nodeCall('role:mtwoB,cmd:ping', '{"k":3}')
    ]);
    check(a.ev === 'result' && a.result.msg.echo.k === 1 && b.ev === 'result' && b.result.msg.echo.k === 2 &&
      c && c.ev === 'result' && c.result.msg.echo.k === 3, 'second pins', { a, b, c });
    ok('the second pin of each answers: meta client to Node and to meta, Node client to meta');
  }
  {
    // members of its own instance are no targets: mwm-node with two pins
    // does not call itself over the mesh, as mesh.js ignores itself
    const self = events.filter((e) => e.from === 'mtwo' && (e.ev === 'add' || e.ev === 'target-add') &&
      /mtwo/.test(e.pin));
    check(self.length === 0, 'mwm-node reports its own other listen', self);
    ok('mwm-node with two pins passes over its own members, as mesh.js does');
  }
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
