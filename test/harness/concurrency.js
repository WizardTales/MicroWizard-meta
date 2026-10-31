// Many calls at once over one connection, both ways, all on 127.0.0.1:
// a Node service runs any number of calls concurrently over the single
// connection a Node caller keeps per target, and so must mwm-node - with a
// cap on the calls running in the process (--max-inflight) above which it
// answers retry_later_err_overload the way a Node service signals overload.
//
//   node concurrency.js [path/to/mwm-node]
//
// 1. a Node base, the Node echo (role:echo), a meta echo (role:meta), a meta
//    echo capped at CAP calls (role:capped), a meta client (--client) and
//    this process as a Node caller come up
// 2. the Node caller sends BURST calls at once to the meta echo - and, as
//    the reference, to the Node echo - each delay with a random 0-500 ms and
//    a token of its own: every answer carries its own token, they arrive in
//    the order they finish rather than the order they were sent, and the
//    whole burst takes about the longest delay, not the sum
// 3. eight calls of 2 s and then a ping: the ping is back at once
// 4. the same burst as raw ndjson lines on one socket to the meta echo:
//    exactly one whole line per request, no line broken by another
// 5. the meta client sends the burst, a task per call, to the Node echo and
//    to the meta echo: the same three checks
// 6. the cap: CAP + 10 slow lines on one socket to the capped echo - CAP
//    answers, 10 refusals at once with error.details.message
//    retry_later_err_overload; then the Node caller and the meta client each
//    send more than the cap at once: every call settles, the excess as
//    overload (no-current-target or all-targets-overloaded), nothing hangs,
//    and once the circuit closes again a call goes through
// 7. SIGTERM with calls running: every running call is answered, a new one
//    gets the closing answer (no details), and the process ends about half a
//    second after the last answer
//
// Exits non-zero at the first step that does not happen.
import { spawn } from 'node:child_process';
import { EventEmitter } from 'node:events';
import net from 'node:net';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';
import MicroWizard from 'microwizard';
import { start } from './lib/node.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const node = path.resolve(process.argv[2] || path.join(here, '../../build/mwm-node'));
const BASE_PORT = Number(process.env.MWM_BASE_PORT || 38999);
const TAG = process.env.MWM_TAG || 'mwmtest';
const verbose = !!process.env.MWM_VERBOSE;
const BURST = 200;
const SPREAD = 500;
const CAP = 20;

// microwizard puts a listener per call on a connection that is still being
// made; a burst is far more than ten of them
EventEmitter.defaultMaxListeners = BURST * 2;

// fixed SWIM and TCP ports for the meta nodes, below the base in 38xxx and
// clear of the other scripts' (-7 .. -20)
const PORTS = {
  meta: [BASE_PORT - 30, BASE_PORT - 31],
  capped: [BASE_PORT - 32, BASE_PORT - 33],
  client: [BASE_PORT - 34, BASE_PORT - 35],
  stopping: [BASE_PORT - 36, BASE_PORT - 37]
};

for (const [swim, tcp] of Object.values(PORTS)) {
  for (const p of [swim, tcp]) {
    if (p < 38000 || p > 38999) throw new Error(`port ${p} is outside 38xxx`);
  }
}

const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => !k.startsWith('wzdc_')));
const children = new Set();
process.on('exit', () => children.forEach((c) => c.kill('SIGKILL')));
const events = [];
const waiters = [];
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const now = () => performance.now();

function track (label, child) {
  children.add(child);
  child.on('exit', (code, signal) => {
    children.delete(child);
    const e = { from: label, ev: 'exit', code, signal, at: now() };
    events.push(e);
    for (const w of [...waiters]) if (w.match(e)) { waiters.splice(waiters.indexOf(w), 1); w.resolve(e); }
  });
  readline.createInterface({ input: child.stdout }).on('line', (line) => {
    let e;
    try {
      e = JSON.parse(line);
    } catch (err) {
      if (verbose) console.error(`[${label}] ${line}`);
      return;
    }
    e.from = label;
    e.at = now();
    events.push(e);
    if (verbose && e.ev !== 'result' && e.ev !== 'error') console.error(`[${label}]`, JSON.stringify(e));
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

function metaNode (label, [swim, tcp], ...args) {
  return track(label, spawn(node, [
    '--bases', `127.0.0.1:${BASE_PORT}`, '--tag', TAG,
    '--port', String(swim), '--listen-port', String(tcp), '--events', ...args
  ], { env, stdio: ['pipe', 'pipe', 'pipe'] }));
}

let step = 0;
const ok = (what) => console.log(`ok ${++step} - ${what}`);
function check (good, what, detail) {
  if (!good) throw new Error(`${what}: ${typeof detail === 'string' ? detail : JSON.stringify(detail)}`);
}

// a burst: BURST calls, each its own token and a random delay
function burst (n = BURST) {
  return Array.from({ length: n }, (_, i) => ({
    token: `t${i}-${Math.random().toString(36).slice(2, 8)}`,
    ms: Math.floor(Math.random() * (SPREAD + 1))
  }));
}

/**
 * What every burst must show: its own token on every answer, none lost,
 * none twice, the answers in the order the calls finished (an inversion is
 * two calls whose delays differ by more than `slack` arriving the wrong way
 * round), not in the order they were sent, and the whole in about the
 * longest delay.
 */
function judge (what, calls, outcomes, wall, slack = 60) {
  const longest = Math.max(...calls.map((c) => c.ms));
  const seen = new Set();
  for (const [i, o] of outcomes.entries()) {
    check(o && o.token === calls[i].token, `${what}: call ${i} came back with another token`, o);
    check(!seen.has(o.token), `${what}: token twice`, o.token);
    seen.add(o.token);
  }
  check(seen.size === calls.length, `${what}: answers lost`, `${seen.size} of ${calls.length}`);

  const byArrival = calls.map((c, i) => ({ ...c, i, at: outcomes[i].at })).sort((a, b) => a.at - b.at);
  let inversions = 0;
  for (let a = 0; a < byArrival.length; ++a) {
    for (let b = a + 1; b < byArrival.length; ++b) {
      if (byArrival[a].ms - byArrival[b].ms > slack) ++inversions;
    }
  }
  check(inversions === 0, `${what}: answers out of the order they finished`, `${inversions} inversions`);
  const inRequestOrder = byArrival.every((c, k) => c.i === k);
  check(!inRequestOrder, `${what}: answers came back in the order they were sent`, '');
  check(wall < longest + 600, `${what}: the burst took the sum, not the longest`, `${Math.round(wall)} ms, longest delay ${longest} ms`);
  return `${calls.length} calls, longest delay ${longest} ms, all back in ${Math.round(wall)} ms, in the order they finished`;
}

// raw ndjson on one socket: lines out, every line that comes back
function rawSocket (port) {
  const socket = net.connect(port, '127.0.0.1');
  const lines = [];
  const listeners = [];
  readline.createInterface({ input: socket }).on('line', (line) => {
    const got = { line, at: now() };
    lines.push(got);
    for (const l of [...listeners]) l(got);
  });
  return {
    ready: new Promise((resolve, reject) => { socket.once('connect', resolve); socket.once('error', reject); }),
    send: (obj) => socket.write(JSON.stringify(obj) + '\n'),
    lines,
    until: (n, ms) => new Promise((resolve, reject) => {
      if (lines.length >= n) return resolve(lines);
      const timer = setTimeout(() => reject(new Error(`${lines.length} of ${n} lines in ${ms} ms`)), ms);
      listeners.push(() => { if (lines.length >= n) { clearTimeout(timer); resolve(lines); } });
    }),
    end: () => socket.destroy()
  };
}

const request = (id, role, cmd, args) => ({ id, k: 'a', sync: true, args: { role, cmd, ...args } });

// the meta client: a JSON line per call on stdin, the outcome by tag
let client;
let tags = 0;
function ask (call, ms = 20000) {
  const tag = ++tags;
  client.stdin.write(JSON.stringify({ tag, ...call }) + '\n');
  return waitFor(`meta client: answer to ${JSON.stringify(call)}`,
    (e) => e.from === 'client' && e.tag === tag && (e.ev === 'result' || e.ev === 'error'), ms);
}

async function main () {
  // 1 ------------------------------------------------------------------
  track('base', spawn(process.execPath, [path.join(here, 'base.js'), 'base'], {
    env: { ...env, MWM_BASE_PORT: String(BASE_PORT), MWM_TAG: TAG }, stdio: ['ignore', 'pipe', 'pipe']
  }));
  await waitFor('base ready', (e) => e.from === 'base' && e.ev === 'ready', 5000);
  track('echo', spawn(process.execPath, [path.join(here, 'echo.js'), 'echo'], {
    env: { ...env, MWM_BASE_PORT: String(BASE_PORT), MWM_TAG: TAG }, stdio: ['ignore', 'pipe', 'pipe']
  }));
  metaNode('meta', PORTS.meta, '--pin', 'role:meta,cmd:*', '--echo', 'meta');
  metaNode('capped', PORTS.capped, '--pin', 'role:capped,cmd:*', '--echo', 'capped',
    '--max-inflight', String(CAP));
  client = metaNode('client', PORTS.client, '--client', '--no-patient');
  await waitFor('echo ready', (e) => e.from === 'echo' && e.ev === 'ready', 5000);
  for (const label of ['meta', 'capped', 'client']) {
    await waitFor(`${label} joined`, (e) => e.from === label && e.ev === 'joined', 5000);
  }
  for (const pin of ['role:echo,cmd:*', 'role:meta,cmd:*', 'role:capped,cmd:*']) {
    await waitFor(`the meta client has ${pin}`,
      (e) => e.from === 'client' && e.ev === 'target-add' && e.pin === pin, 6000);
  }

  // this process as a Node caller, microwizard 1.5.0 as core has it: one
  // connection per target, every call over it
  const mc = await start('caller', {}, new MicroWizard());
  const deadline = Date.now() + 8000;
  while (!['role:echo,cmd:delay', 'role:meta,cmd:delay', 'role:capped,cmd:delay'].every((p) => mc.find(p))) {
    check(Date.now() < deadline, 'the Node caller has no targets', '');
    await sleep(100);
  }
  ok('base, Node echo, meta echo, capped meta echo, meta client and a Node caller are up');

  // 2 ------------------------------------------------------------------
  const nodeBurst = async (role, calls = burst()) => {
    const began = now();
    const outcomes = await Promise.all(calls.map((c) =>
      mc.act(`role:${role},cmd:delay`, { ms: c.ms, token: c.token })
        .then((r) => ({ token: r?.msg?.echo?.token, at: now() }), (e) => ({ error: e, at: now() }))));
    return { calls, outcomes, wall: now() - began };
  };
  for (const role of ['echo', 'meta']) {
    const { calls, outcomes, wall } = await nodeBurst(role);
    ok(`Node caller -> ${role === 'meta' ? 'meta' : 'Node'} echo: ${judge(`Node -> ${role}`, calls, outcomes, wall)}`);
  }

  // 3 ------------------------------------------------------------------
  {
    const slow = Array.from({ length: 8 }, (_, i) => mc.act('role:meta,cmd:delay', { ms: 2000, token: `slow${i}` }));
    await sleep(100);
    const began = now();
    const fast = await mc.act('role:meta,cmd:ping', { n: 1 });
    const took = now() - began;
    check(fast?.code === 200 && took < 300, 'a ping behind eight slow calls waited', `${Math.round(took)} ms`);
    const late = await Promise.all(slow);
    check(late.every((r, i) => r?.msg?.echo?.token === `slow${i}`), 'the slow calls', late);
    ok(`eight 2 s calls running, a ping behind them is back in ${Math.round(took)} ms - no head-of-line wait`);
  }

  // 4 ------------------------------------------------------------------
  {
    const sock = rawSocket(PORTS.meta[1]);
    await sock.ready;
    const calls = burst();
    const began = now();
    calls.forEach((c, i) => sock.send(request(`r${i}`, 'meta', 'delay', { ms: c.ms, token: c.token })));
    await sock.until(BURST, 5000);
    await sleep(200);
    const wall = Math.max(...sock.lines.map((l) => l.at)) - began;
    check(sock.lines.length === BURST, 'raw: lines', `${sock.lines.length} for ${BURST} requests`);
    const parsed = sock.lines.map((l) => { try { return { ...JSON.parse(l.line), at: l.at }; } catch (e) { return null; } });
    check(parsed.every((p) => p && p.k === 'res'), 'raw: a line is not one whole answer', sock.lines.find((l, i) => !parsed[i]));
    const byId = new Map(parsed.map((p) => [p.id, p]));
    check(byId.size === BURST, 'raw: an id twice or one missing', byId.size);
    const outcomes = calls.map((c, i) => {
      const p = byId.get(`r${i}`);
      return p && { token: p.res?.msg?.echo?.token, at: p.at };
    });
    ok(`raw ndjson -> meta echo, one socket: one whole line per request; ${judge('raw -> meta', calls, outcomes, wall)}`);
    sock.end();
  }

  // 5 ------------------------------------------------------------------
  for (const role of ['echo', 'meta']) {
    const calls = burst();
    const began = now();
    const outcomes = await Promise.all(calls.map((c) =>
      ask({ act: `role:${role},cmd:delay`, args: { ms: c.ms, token: c.token } })
        .then((e) => ({ token: e.result?.msg?.echo?.token, at: e.at, e }))));
    const wall = now() - began;
    ok(`meta client (a task per call) -> ${role === 'meta' ? 'meta' : 'Node'} echo: ${judge(`meta client -> ${role}`, calls, outcomes, wall)}`);
  }

  // 6 ------------------------------------------------------------------
  {
    const sock = rawSocket(PORTS.capped[1]);
    await sock.ready;
    const began = now();
    for (let i = 0; i < CAP + 10; ++i) sock.send(request(`c${i}`, 'capped', 'delay', { ms: 600, token: `c${i}` }));
    await sock.until(CAP + 10, 5000);
    const parsed = sock.lines.map((l) => ({ ...JSON.parse(l.line), at: l.at - began }));
    const refused = parsed.filter((p) => p.error);
    const answered = parsed.filter((p) => p.res);
    check(answered.length === CAP && refused.length === 10, 'cap: answers and refusals',
      { answered: answered.length, refused: refused.length });
    check(refused.every((p) => p.at < 300), 'cap: a refusal waited', refused.map((p) => Math.round(p.at)));
    const shape = JSON.stringify(refused[0].error);
    check(shape === JSON.stringify({ message: 'retry_later_err_overload', name: 'Error', details: { message: 'retry_later_err_overload' } }),
      'cap: the refusal\'s shape', shape);
    check(refused.every((p) => /^c(2\d)$/.test(p.id)), 'cap: the refused are not the last ones', refused.map((p) => p.id));
    // the cap is free again once they are done
    sock.lines.length = 0;
    for (let i = 0; i < CAP; ++i) sock.send(request(`d${i}`, 'capped', 'delay', { ms: 10, token: `d${i}` }));
    await sock.until(CAP, 5000);
    check(sock.lines.every((l) => JSON.parse(l.line).res), 'cap: not given back', sock.lines.map((l) => l.line));
    sock.end();
    ok(`--max-inflight ${CAP}: ${CAP + 10} slow calls on one socket, ${CAP} answered, 10 refused at once with ${shape}; afterwards the cap is free again`);
  }

  {
    // a Node caller: the refusal counts as overload, the circuit opens, the
    // excess settles as no-current-target or all-targets-overloaded
    const began = now();
    const settled = await Promise.all(Array.from({ length: CAP * 3 }, (_, i) =>
      mc.act('role:capped,cmd:delay', { ms: 400, token: `n${i}` })
        .then((r) => ({ ok: r?.msg?.echo?.token === `n${i}` }), (e) => ({ err: typeof e.err === 'string' ? e.err : e.err?.message }))));
    const took = now() - began;
    const good = settled.filter((s) => s.ok).length;
    const errs = [...new Set(settled.filter((s) => !s.ok).map((s) => s.err))];
    check(good >= 1 && good <= CAP && good + settled.filter((s) => s.err).length === CAP * 3, 'cap, Node caller: outcomes', settled);
    check(errs.every((e) => e === 'no-current-target' || e === 'all-targets-overloaded'), 'cap, Node caller: errors', errs);
    check(took < 4000, 'cap, Node caller: took too long', `${Math.round(took)} ms`);

    // the meta client the same way
    await sleep(1500);
    const mBegan = now();
    const mSettled = await Promise.all(Array.from({ length: CAP * 3 }, (_, i) =>
      ask({ act: 'role:capped,cmd:delay', args: { ms: 400, token: `m${i}` } }, 10000)));
    const mTook = now() - mBegan;
    const mGood = mSettled.filter((e) => e.ev === 'result' && e.result?.msg?.echo?.token).length;
    const mErrs = [...new Set(mSettled.filter((e) => e.ev === 'error').map((e) => typeof e.error.err === 'string' ? e.error.err : e.error.err?.message))];
    check(mGood >= 1 && mGood <= CAP, 'cap, meta client: answers', mSettled.map((e) => e.ev));
    check(mErrs.every((e) => e === 'no-current-target' || e === 'all-targets-overloaded'), 'cap, meta client: errors', mErrs);
    check(mTook < 4000, 'cap, meta client: took too long', `${Math.round(mTook)} ms`);

    // and once the circuit is closed again, calls go through
    let back = null;
    const until = Date.now() + 8000;
    while (!back && Date.now() < until) {
      await sleep(250);
      back = await mc.act('role:capped,cmd:ping', { n: 2 }).then((r) => r?.code === 200, () => false) || null;
    }
    check(back, 'cap: the capped echo never answered again', '');
    ok(`over the cap, Node caller (${good} answered, the rest ${errs.join('/')}, ${Math.round(took)} ms) and meta client (${mGood} answered, the rest ${mErrs.join('/')}, ${Math.round(mTook)} ms): every call settles, then calls go through again`);
  }

  // 7 ------------------------------------------------------------------
  {
    const stopping = metaNode('stopping', PORTS.stopping, '--pin', 'role:stopping,cmd:*', '--echo', 'stopping');
    await waitFor('stopping joined', (e) => e.from === 'stopping' && e.ev === 'joined', 5000);
    const sock = rawSocket(PORTS.stopping[1]);
    await sock.ready;
    for (let i = 0; i < 20; ++i) sock.send(request(`s${i}`, 'stopping', 'delay', { ms: 1200, token: `s${i}` }));
    await sleep(200);
    stopping.kill('SIGTERM');
    await sleep(100);
    sock.send(request('late', 'stopping', 'delay', { ms: 0, token: 'late' }));
    await sock.until(21, 5000);
    const parsed = sock.lines.map((l) => ({ ...JSON.parse(l.line), at: l.at }));
    const late = parsed.find((p) => p.id === 'late');
    check(late && JSON.stringify(late.error) === JSON.stringify({ message: 'retry_later_err_overload', name: 'Error' }),
      'SIGTERM: a new call is not refused the closing way', late);
    const done = parsed.filter((p) => p.id !== 'late');
    check(done.length === 20 && done.every((p) => p.res?.msg?.echo?.token === p.id), 'SIGTERM: running calls not finished', done);
    const lastAnswer = Math.max(...done.map((p) => p.at));
    const exit = await waitFor('stopping exits', (e) => e.from === 'stopping' && e.ev === 'exit', 5000);
    const after = exit.at - lastAnswer;
    check(after > 450 && after < 1200, 'SIGTERM: the end is not half a second after the last answer', `${Math.round(after)} ms`);
    check(exit.code === 0, 'SIGTERM: exit code', exit);
    sock.end();
    ok(`SIGTERM with 20 calls running: all 20 answered, a new call refused ${JSON.stringify(late.error)}, the end ${Math.round(after)} ms after the last answer`);
  }

  await mc.close().catch(() => {});
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
