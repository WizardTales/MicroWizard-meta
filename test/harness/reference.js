// What a Node caller gets back from a service - the Node echo service
// (echo.js, role:echo) and the meta one (mwm-node --echo meta, role:meta),
// case by case, side by side. Both have to give the same answer, and the
// answer has to be the one docs/protocol.md describes; for each case the
// gateway's view (actAsync().then(defaultError), graphql-js) is compared
// as well.
//
//   node reference.js [path/to/mwm-node]
//
// Without a mwm-node (or with MWM_NODE_ONLY=1) only the Node side runs.
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const mwmNode = path.resolve(process.argv[2] || path.join(here, '../../build/mwm-node'));
const withMeta = !process.env.MWM_NODE_ONLY && fs.existsSync(mwmNode);
const BASE_PORT = Number(process.env.MWM_BASE_PORT || 38999);
const TAG = process.env.MWM_TAG || 'mwmtest';
const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => !k.startsWith('wzdc_')));
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const boot = (script, ...args) =>
  spawn(process.execPath, [path.join(here, script), ...args], { env, stdio: 'ignore' });
const bootMeta = (port) =>
  spawn(mwmNode, ['--bases', `127.0.0.1:${BASE_PORT}`, '--tag', TAG, '--pin', 'role:meta,cmd:*',
    '--echo', 'meta', '--port', String(port)], { env, stdio: 'ignore' });

const call = (pattern, args = '{}', extra = {}) => {
  const out = spawnSync(process.execPath, [path.join(here, 'client.js'), 'ref', pattern, args], {
    env: { ...env, MWM_CLIENT_WAIT: '10000', ...extra },
    encoding: 'utf8',
    timeout: 30000
  });
  return out.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l)).find((e) => e.ev === 'result' || e.ev === 'error');
};

// what may differ between the two services: their role and their name
// and the stack trace, which only says where each was thrown
const same = (e) => JSON.stringify(e, (k, v) =>
  k === 't' || k === 'pattern' ? undefined
    : k === 'message' && typeof v === 'string' && v.startsWith('Unexpected error value')
      ? v.replace(/ at .*/, '').replace(/role: '(echo|meta)'/g, "role: 'ROLE'")
    : v === 'echo' || v === 'meta' ? 'ROLE'
      : typeof v === 'string' ? v.replace(/role: '(echo|meta)'/g, "role: 'ROLE'") : v);

const cases = [
  ['answer', 'ping', '{"n":1}',
    (e) => e.ev === 'result' && e.result.code === 200 && e.result.msg.echo.n === 1 &&
      e.result.msg.echo.cmd === 'ping' && e.gql.data && e.gql.data.code === 200],
  ['an application error is an answer, and a ValidationError in the gateway', 'fail', '{}',
    (e) => e.ev === 'result' && e.result.code === 409 && e.result.err === 'conflict on purpose' &&
      e.gql.message === 'conflict on purpose' && e.gql.extensions.code === 'CONFLICT' &&
      e.gql.code === 409 && e.gql.state.err === 'conflict on purpose'],
  ['a thrown error rejects with {err: Error, msg}, which graphql-js calls an unexpected value', 'throw', '{}',
    (e) => e.ev === 'error' && !e.error.isError && e.error.err.message === 'thrown on purpose' &&
      e.error.err.code === 'E_PURPOSE' && e.gql.notAnError && e.gql.message.startsWith('Unexpected error value')],
  ['overload with one target: no-current-target, no ten retries', 'overload', '{}',
    (e) => e.ev === 'error' && e.error.err === 'no-current-target'],
  ['a pin with no handler behind the wildcard resolves undefined', 'nothing', '{}',
    (e) => e.ev === 'result' && e.result === undefined],
  ['past timeout (5555 ms) with one target: no-current-target', 'slow', '{"ms":7000}',
    (e) => e.ev === 'error' && e.error.err === 'no-current-target']
];

const children = [];
process.on('exit', () => children.forEach((c) => c.kill('SIGKILL')));
children.push(boot('base.js'));
await sleep(800);
const echo = boot('echo.js');
children.push(echo);
let meta = null;
if (withMeta) {
  meta = bootMeta(BASE_PORT - 7);
  children.push(meta);
}
await sleep(1500);

let failed = 0;
let n = 0;
const report = (good, what, detail) => {
  failed += good ? 0 : 1;
  console.log(`${good ? 'ok' : 'not ok'} ${++n} - ${what}${good ? '' : `: ${detail}`}`);
};

for (const [what, cmd, args, check] of cases) {
  const node = call(`role:echo,cmd:${cmd}`, args);
  const nodeGood = node && check(node);
  if (!withMeta) {
    report(nodeGood, `Node: ${what}`, JSON.stringify(node));
    continue;
  }
  const mine = call(`role:meta,cmd:${cmd}`, args);
  const mineGood = mine && check(mine);
  const alike = node && mine && same(node) === same(mine);
  report(nodeGood && mineGood && alike, `Node and meta alike: ${what}`,
    `\n  node ${JSON.stringify(node)}\n  meta ${JSON.stringify(mine)}`);
}

// last, because it ends the services: a call that arrives while the
// service closes gets retry_later_err_overload - as error.message, without
// details, so the caller does not try elsewhere
const closing = (e) => e && e.ev === 'error' && e.error.err &&
  e.error.err.message === 'retry_later_err_overload';
const nodeClosing = call('role:echo,cmd:ping', '{}', { MWM_SIGNAL: String(echo.pid) });
if (withMeta) {
  const metaClosing = call('role:meta,cmd:ping', '{}', { MWM_SIGNAL: String(meta.pid) });
  report(closing(nodeClosing) && closing(metaClosing) && same(nodeClosing) === same(metaClosing),
    'Node and meta alike: a call into a closing service is answered retry_later_err_overload',
    `\n  node ${JSON.stringify(nodeClosing)}\n  meta ${JSON.stringify(metaClosing)}`);
} else {
  report(closing(nodeClosing), 'Node: a call into a closing service is answered retry_later_err_overload',
    JSON.stringify(nodeClosing));
}
console.log(`1..${n}`);

for (const c of children) c.kill('SIGKILL');
process.exit(failed ? 1 : 0);
