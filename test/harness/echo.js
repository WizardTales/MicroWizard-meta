// A Node service in the shape of user-service: one listen with a wildcard
// pin, consume model, tcp. The answers follow the house convention the
// a gateway reads ({code, msg} or {code >= 400, err}).
//
// MWM_ECHO_PINS='role:a,cmd:*;role:b,cmd:*' makes it a service with one
// listen per pin - each its own SWIM member, as microwizard does it.
import { start, stopOn } from './lib/node.js';

const label = process.argv[2] || 'echo';
const pins = (process.env.MWM_ECHO_PINS || process.env.MWM_ECHO_PIN || 'role:echo,cmd:*').split(';');

const { Watch } = await import('./lib/node.js');
const mc = new Watch(label);

for (const pin of pins) serve(pin.match(/role:([^,]+)/)[1]);

function serve (role) {
  mc.add(`role:${role},cmd:ping`, async function (msg) {
    return { code: 200, msg: { echo: msg, from: label } };
  });

  mc.add(`role:${role},cmd:fail`, async function (msg) {
    return { code: 409, err: 'conflict on purpose' };
  });

  mc.add(`role:${role},cmd:throw`, async function (msg) {
    const err = new Error('thrown on purpose');
    err.code = 'E_PURPOSE';
    throw err;
  });

  mc.add(`role:${role},cmd:overload`, async function (msg) {
    const err = new Error('overloaded');
    err.details = { message: 'retry_later_err_overload' };
    throw err;
  });

  mc.add(`role:${role},cmd:slow`, async function (msg) {
    await new Promise((resolve) => setTimeout(resolve, Number(msg.ms || 6000)));
    return { code: 200, msg: 'late' };
  });

  // ping after msg.ms, for the concurrency tests
  mc.add(`role:${role},cmd:delay`, async function (msg) {
    const ms = Number(msg.ms || 0);
    if (ms > 0) await new Promise((resolve) => setTimeout(resolve, ms));
    return { code: 200, msg: { echo: msg, from: label } };
  });

  // msg.n rounds of xorshift64, as echo.c's burn: CPU-bound calls
  mc.add(`role:${role},cmd:burn`, async function (msg) {
    let x = 0x9E3779B97F4A7C15n;
    const mask = (1n << 64n) - 1n;
    for (let i = 0, n = Number(msg.n ?? 500000); i < n; ++i) {
      x ^= (x << 13n) & mask;
      x ^= x >> 7n;
      x ^= (x << 17n) & mask;
    }
    return { code: 200, msg: { echo: msg, from: label, h: Number(x >> 12n) } };
  });
}

await start(label, {
  listen: pins.map((pin) => ({ pin, model: 'consume', type: 'tcp' }))
}, mc);
stopOn(mc, label);
