// Throughput and latency of mwm-node under many concurrent calls, with one
// carrier and with the runtime's default, for handlers that wait and
// handlers that burn CPU - the service side over raw ndjson, and the meta
// client (--client) calling a meta echo.
//
//   node test/bench/bench.mjs [path/to/mwm-node] [options]
//
//     --seconds S        measured time per run (3), after 1 s of warm-up
//     --carriers LIST    META_CARRIERS values, "default" for unset (1,default)
//     --handlers LIST    ping, delay (10 ms sleep), burn (n rounds, ~1 ms)
//     --inflight LIST    calls kept in flight at once (16,256,2048)
//     --connections N    sockets the service load is spread over (32)
//     --workers N        load processes (4)
//     --burn N           rounds of xorshift64 per burn call (500000, ~1 ms)
//     --server-cpus L    taskset list for mwm-node (0-11), "" for none
//     --load-cpus L      taskset list for the load (12-15), "" for none
//     --only service|client
//     --json             all rows as JSON at the end
//
// Everything on 127.0.0.1, ports 387xx: no base is needed for the service
// runs (the node's SWIM sends to 38799, where nobody is); the client runs
// start mwm-node --isbase there, a meta echo and a meta client.
//
// Each run is closed-loop: a call is sent as soon as one comes back, so the
// in-flight count holds. Reported: calls/s, p50 and p99 in ms, mwm-node's
// CPU (100 % = one core, perf stat task-clock), its busiest thread and how
// many of its threads did 2 % of a core or more, and the load's CPU - to
// tell the node's limit from the load's.
import { spawn, fork } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// ------------------------------------------------------------ load worker
if (process.argv[2] === '--worker') {
  const [port, conns, inflight, handler, burnN, seconds] = process.argv.slice(3).map((v, i) => i === 3 ? v : Number(v));
  const args = handler === 'delay' ? { role: 'meta', cmd: 'delay', ms: 10 }
    : handler === 'burn' ? { role: 'meta', cmd: 'burn', n: burnN }
      : { role: 'meta', cmd: 'ping' };
  const latencies = [];
  let measuring = false;
  let stopping = false;
  let ids = 0;
  const sockets = [];
  for (let c = 0; c < conns; ++c) {
    const window = Math.floor(inflight / conns) + (c < inflight % conns ? 1 : 0);
    if (window === 0) continue;
    const s = net.connect(port, '127.0.0.1');
    s.setNoDelay(true);
    const sent = new Map();
    const send = () => {
      const id = `${process.pid}-${++ids}`;
      sent.set(id, performance.now());
      s.write(JSON.stringify({ id, k: 'a', sync: true, args }) + '\n');
    };
    readline.createInterface({ input: s }).on('error', (e) => {
      if (!stopping) process.stderr.write(`# load: ${e.message} while running\n`);
    }).on('line', (line) => {
      const id = line.slice(7, line.indexOf('"', 7));
      const t = sent.get(id);
      if (t === undefined) return;
      sent.delete(id);
      if (measuring) latencies.push(performance.now() - t);
      if (!stopping) send();
    });
    s.on('connect', () => { for (let i = 0; i < window; ++i) send(); });
    s.on('error', () => {});
    sockets.push(s);
  }
  await sleep(1000);
  measuring = true;
  const began = performance.now();
  await sleep(seconds * 1000);
  measuring = false;
  stopping = true;
  const took = performance.now() - began;
  sockets.forEach((s) => s.destroy());
  process.send({ latencies, took }, () => process.exit(0));
  await new Promise(() => {}); // the rest of this file is the parent's
}

// ------------------------------------------------------------ options
const argv = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 ? argv[i + 1] : fallback;
};
const node = path.resolve(argv.find((a, i) => !a.startsWith('--') && (i === 0 || !argv[i - 1].startsWith('--'))) ||
  path.join(here, '../../build/mwm-node'));
const SECONDS = Number(opt('seconds', 3));
const CARRIERS = opt('carriers', '1,default').split(',');
const HANDLERS = opt('handlers', 'ping,delay,burn').split(',');
const INFLIGHT = opt('inflight', '16,256,2048').split(',').map(Number);
const CONNECTIONS = Number(opt('connections', 32));
const WORKERS = Number(opt('workers', 4));
const BURN = Number(opt('burn', 500000));
const SERVER_CPUS = opt('server-cpus', '0-11');
const LOAD_CPUS = opt('load-cpus', '12-15');
const ONLY = opt('only', '');
const BASE = 38799;
const ECHO = [38790, 38791];
const CLIENT = [38792, 38793];

for (const p of [BASE, ...ECHO, ...CLIENT]) {
  if (p < 38000 || p > 38999) throw new Error(`port ${p} is outside 38xxx`);
}

const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => !k.startsWith('wzdc_')));
const children = new Set();
process.on('exit', () => children.forEach((c) => c.kill('SIGKILL')));
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => process.exit(1));

function pinned (cpus, cmd, args, options) {
  const child = cpus ? spawn('taskset', ['-c', cpus, cmd, ...args], options) : spawn(cmd, args, options);
  children.add(child);
  child.on('exit', () => children.delete(child));
  return child;
}

function metaNode (carriers, swim, tcp, ...args) {
  const e = { ...env };
  if (carriers !== 'default') e.META_CARRIERS = carriers;
  else delete e.META_CARRIERS;
  return pinned(SERVER_CPUS, node, ['--bases', `127.0.0.1:${BASE}`, '--tag', 'mwmbench',
    '--port', String(swim), '--listen-port', String(tcp), '--max-inflight', '1000000', ...args],
  { env: e, stdio: ['pipe', 'pipe', 'ignore'] });
}

/**
 * CPU per thread over the next `seconds`, from perf stat's task-clock:
 * /proc's utime and stime undercount threads badly on this kernel
 * (NO_HZ_FULL, VIRT_CPU_ACCOUNTING_GEN) - a carrier perf saw at 100 % was
 * 28 % there. Answers {cpu, top, used, threads} in % of one core.
 */
function cpuOver (pids, seconds) {
  return new Promise((resolve) => {
    const perf = spawn('perf', ['stat', '--per-thread', '-x,', '-e', 'task-clock', '-p', pids.join(','), '--', 'sleep', String(seconds)],
      { env: { ...env, LC_ALL: 'C' }, stdio: ['ignore', 'ignore', 'pipe'] });
    let out = '';
    perf.stderr.on('data', (d) => { out += d; });
    perf.on('exit', () => {
      let total = 0;
      let top = 0;
      let used = 0;
      let threads = 0;
      for (const line of out.split('\n')) {
        const f = line.split(',');
        if (f.length < 4 || !f[3].startsWith('task-clock')) continue;
        ++threads;
        const share = (Number(f[1]) || 0) / (seconds * 1000);
        total += share;
        top = Math.max(top, share);
        if (share >= 0.02) ++used;
      }
      resolve({ cpu: Math.round(total * 100), top: Math.round(top * 100), used, threads });
    });
  });
}

const pct = (sorted, p) => sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] : NaN;
function summary (latencies, took) {
  const sorted = Float64Array.from(latencies).sort();
  return {
    rate: Math.round(latencies.length / (took / 1000)),
    p50: pct(sorted, 0.5),
    p99: pct(sorted, 0.99)
  };
}

const rows = [];
function report (row) {
  rows.push(row);
  const f = (v) => (v < 10 ? v.toFixed(2) : v.toFixed(1)).padStart(7);
  console.log(`${row.side.padEnd(8)} carriers=${String(row.carriers).padEnd(8)} ${row.handler.padEnd(6)} inflight=${String(row.inflight).padStart(5)}  ` +
    `${String(row.rate).padStart(7)} calls/s  p50 ${f(row.p50)} ms  p99 ${f(row.p99)} ms  ` +
    `node ${String(row.node.cpu).padStart(4)} % (busiest thread ${String(row.node.top).padStart(3)} %, ${row.node.used}/${row.node.threads} threads used)  load ${String(row.load).padStart(4)} %`);
}

async function waitPort (port, ms = 5000) {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    const okay = await new Promise((resolve) => {
      const s = net.connect(port, '127.0.0.1');
      s.on('connect', () => { s.destroy(); resolve(true); });
      s.on('error', () => resolve(false));
    });
    if (okay) return;
    await sleep(50);
  }
  throw new Error(`nothing on ${port}`);
}

async function stop (child) {
  if (!child || child.exitCode !== null) return;
  child.kill('SIGKILL');
  await new Promise((resolve) => child.once('exit', resolve));
}

// ------------------------------------------------------------ service runs
async function serviceRun (carriers, handler, inflight) {
  const echo = metaNode(carriers, ECHO[0], ECHO[1], '--pin', 'role:meta,cmd:*', '--echo', 'meta');
  await waitPort(ECHO[1]);
  const pid = echo.pid; // taskset execs, so it is mwm-node's
  const conns = Math.min(CONNECTIONS, inflight);
  const workers = Math.min(WORKERS, conns);
  const loads = [];
  for (let w = 0; w < workers; ++w) {
    const c = Math.floor(conns / workers) + (w < conns % workers ? 1 : 0);
    const f = Math.floor(inflight / workers) + (w < inflight % workers ? 1 : 0);
    const args = ['--worker', ECHO[1], c, f, handler, BURN, SECONDS].map(String);
    const child = LOAD_CPUS
      ? fork(fileURLToPath(import.meta.url), args, { execPath: 'taskset', execArgv: ['-c', LOAD_CPUS, process.execPath] })
      : fork(fileURLToPath(import.meta.url), args);
    loads.push(child);
  }
  const results = Promise.all(loads.map((l) => new Promise((resolve) => l.once('message', resolve))));
  await sleep(1100);
  const [nodeCpu, loadCpu] = await Promise.all([cpuOver([pid], SECONDS - 0.3), cpuOver(loads.map((l) => l.pid), SECONDS - 0.3)]);
  await results;
  await stop(echo);
  const all = [];
  const took = (await results)[0].took;
  for (const r of await results) for (const v of r.latencies) all.push(v);
  report({ side: 'service', carriers, handler, inflight, ...summary(all, took), node: nodeCpu, load: loadCpu.cpu });
}

// ------------------------------------------------------------ client runs
async function clientRun (carriers, handler, inflight, echoCarriers = 'default') {
  const base = metaNode('default', BASE, BASE - 1, '--isbase');
  await sleep(300);
  const echo = metaNode(echoCarriers, ECHO[0], ECHO[1], '--pin', 'role:meta,cmd:*', '--echo', 'meta');
  const client = metaNode(carriers, CLIENT[0], CLIENT[1], '--client', '--events', '--timeout', '30000');
  const lines = readline.createInterface({ input: client.stdout });
  const args = handler === 'delay' ? { ms: 10 } : handler === 'burn' ? { n: BURN } : {};
  const act = `role:meta,cmd:${handler}`;
  let ready;
  const targeted = new Promise((resolve) => { ready = resolve; });
  const sent = new Map();
  const latencies = [];
  let measuring = false;
  let stopping = false;
  let tag = 0;
  const send = () => {
    const t = ++tag;
    sent.set(t, performance.now());
    client.stdin.write(`{"tag":${t},"act":"${act}","args":${JSON.stringify(args)}}\n`);
  };
  let errors = 0;
  lines.on('line', (line) => {
    if (line.startsWith('{"ev":"target-add"')) return ready();
    if (!line.startsWith('{"ev":"result"') && !line.startsWith('{"ev":"error"')) return;
    if (line.startsWith('{"ev":"error"')) ++errors;
    const t = Number(line.slice(line.indexOf('"tag":') + 6, line.indexOf(',', line.indexOf('"tag":'))));
    const at = sent.get(t);
    if (at === undefined) return;
    sent.delete(t);
    if (measuring) latencies.push(performance.now() - at);
    if (!stopping) send();
  });
  await Promise.race([targeted, sleep(8000)]);
  await sleep(300);
  for (let i = 0; i < inflight; ++i) send();
  await sleep(1000);
  measuring = true;
  const t0 = performance.now();
  const cpu = Promise.all([cpuOver([client.pid], SECONDS - 0.3), cpuOver([echo.pid], SECONDS - 0.3)]);
  await sleep(SECONDS * 1000);
  measuring = false;
  stopping = true;
  const ms = performance.now() - t0;
  const [nodeCpu, echoCpu] = await cpu;
  await Promise.all([stop(client), stop(echo), stop(base)]);
  report({ side: 'client', carriers, handler, inflight, ...summary(latencies, ms), node: nodeCpu, load: echoCpu.cpu, errors });
  if (errors) console.log(`#   ${errors} calls rejected`);
}

// ------------------------------------------------------------ the matrix
console.log(`# ${os.cpus().length} CPUs (${os.cpus()[0].model}), mwm-node on ${SERVER_CPUS || 'all'}, load on ${LOAD_CPUS || 'all'}, ` +
  `${SECONDS} s per run, burn = ${BURN} rounds`);
if (ONLY !== 'client') {
  console.log('# service: raw ndjson over ' + CONNECTIONS + ' sockets from ' + WORKERS + ' load processes; load % is theirs');
  for (const handler of HANDLERS) {
    for (const carriers of CARRIERS) {
      for (const inflight of INFLIGHT) await serviceRun(carriers, handler, inflight);
    }
  }
}
if (ONLY !== 'service') {
  console.log('# client: mwm-node --client, a task per call from stdin, to a meta echo (default carriers); node % is the client\'s, load % the echo\'s');
  for (const handler of HANDLERS) {
    for (const carriers of CARRIERS) {
      for (const inflight of INFLIGHT) await clientRun(carriers, handler, inflight);
    }
  }
}
if (argv.includes('--json')) console.log(JSON.stringify(rows));
process.exit(0);
