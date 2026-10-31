// The remote suite's MicroWizard tests (suites/remote.js of
// WizardTales/microservice-benchmarks) as one run each: a microwizard
// transport client - the README's MicroWizardActE, MicroWizard1 and
// MicroWizard2 - against HOST:PORT, batches of CONCURRENCY calls, rps =
// batches/s x CONCURRENCY. The microwizard is the benchmark repo's
// (MWB_DIR/node_modules/microwizard).
//
//   node node-client.mjs HOST PORT CONCURRENCY [seconds 5] [actE|act|actStr]
//
// One difference to remote.js: actE sends p as the pin text 'cmd:add'.
// remote.js hands meta.pin on, which microwizard 1.1.1 has as the object
// {cmd:'add'}; a Node 1.1.1 service takes either, the wire of 1.5.0 - which
// mwm-node speaks - has the text.
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const dir = process.env.MWB_DIR || path.resolve(path.dirname(new URL(import.meta.url).pathname), '../../microservice-benchmarks');
const require = createRequire(path.join(dir, 'index.js'));
const mwPath = require.resolve('microwizard');
const { default: Microwizard, transport } = await import(pathToFileURL(mwPath));
const { uid } = require('uid');

const [host, port, c, seconds = '5', mode = 'actE'] = process.argv.slice(2);
const n = Number(c);
const mc2 = new Microwizard({ transport: { timeout: 2000 } });
const tp2 = transport({}, mc2);

const client = await new Promise((resolve, reject) =>
  tp2.client({ host, port: Number(port) }, (err, cl) => (err ? reject(err) : resolve(cl)))
);

mc2.add('cmd:*', async (msg, meta) => {
  if (meta.pin && mode === 'actE') {
    return await client.send(msg, { p: 'cmd:add', id: uid(), k: 'aE' });
  }
  return await client.send(msg, { id: uid() });
});

const call =
  mode === 'act'
    ? () => mc2.act({ cmd: 'add', a: 5, b: 3 })
    : mode === 'actStr'
      ? () => mc2.act('cmd:add', { a: 5, b: 3 })
      : () => mc2.actE('cmd:add', { a: 5, b: 3 });

const lat = [];
let failures = 0;

async function batches (until, record) {
  let calls = 0;
  while (performance.now() < until) {
    const began = performance.now();
    const work = [];
    for (let i = 0; i < n; ++i) {
      work.push(
        call().then(
          () => { if (record && lat.length < 8e6) lat.push(performance.now() - began); },
          () => { ++failures; }
        )
      );
    }
    await Promise.all(work);
    calls += n;
  }
  return calls;
}

await call();
await batches(performance.now() + seconds * 100, false);
failures = 0;
const began = performance.now();
const calls = await batches(began + seconds * 1000, true);
const took = (performance.now() - began) / 1000;
lat.sort((a, b) => a - b);
console.log(JSON.stringify({
  suite: 'remote', client: 'node', mw: require(path.join(path.dirname(mwPath), 'package.json')).version,
  node: process.version, mode, c: n, calls, s: +took.toFixed(3), rps: Math.round(calls / took), failures,
  p50us: +(lat[lat.length >> 1] * 1000).toFixed(1), p99us: +(lat[Math.floor(lat.length * 0.99)] * 1000).toFixed(1)
}));
process.exit(0);
