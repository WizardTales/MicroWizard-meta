// A consumer like a gateway: no pin of its own, joins, waits until the
// pattern has a target and acts it.
//
//   node client.js <label> <pattern> [json-args] [--watch]
//
// Prints one JSON line per answer ({ev:'result'} or {ev:'error'}) and, with
// --watch, stays in the mesh afterwards and reports adds and removes.
import { start, say } from './lib/node.js';
import { defaultError, graphqlView } from './lib/gateway.js';

const [label = 'client', pattern = 'role:echo,cmd:ping', raw = '{}'] =
  process.argv.slice(2).filter((a) => !a.startsWith('--'));
const watch = process.argv.includes('--watch');
const args = JSON.parse(raw);
const deadline = Date.now() + Number(process.env.MWM_CLIENT_WAIT || 10000);

const mc = await start(label, {});

if (pattern !== '-') {
  while (!mc.find(pattern)) {
    if (Date.now() > deadline) {
      say(label, { ev: 'error', kind: 'no-target', pattern });
      process.exit(2);
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }

  // MWM_SIGNAL=<pid>: SIGTERM that process just before the call, to see
  // what a caller gets from a service on its way out
  if (process.env.MWM_SIGNAL) {
    process.kill(Number(process.env.MWM_SIGNAL), 'SIGTERM');
    await new Promise((resolve) => setTimeout(resolve, 50));
  }

  // the gateway's view of the same call: actAsync(...).then(defaultError),
  // and what graphql-js and formatError make of whatever is thrown
  const call = mc.act(pattern, args);
  const gql = await call.then(defaultError).then(
    (data) => ({ data }),
    (e) => graphqlView(e)
  );

  try {
    const result = await call;
    say(label, { ev: 'result', pattern, result, gql });
  } catch (e) {
    say(label, {
      ev: 'error',
      pattern,
      error: {
        isError: e instanceof Error,
        keys: Object.keys(e),
        err: e.err instanceof Error
          ? { message: e.err.message, name: e.err.name, code: e.err.code, keys: Object.keys(e.err) }
          : e.err,
        message: e.message
      },
      gql
    });
  }
}

if (!watch) process.exit(0);
process.on('SIGTERM', () => process.exit(0));
