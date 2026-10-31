// One MicroWizard node for the harness, fenced in to 127.0.0.1.
//
// Every node here takes its bases from MWM_BASE_PORT and nothing else: no
// rc('wzdc'), no DNS seed, no multicast, no guess. A dev mesh often
// sits on 39000/39001 and microwizard defaults to 39999, so the default is
// 38999 and anything that is not 127.0.0.1 is refused before a socket opens.
//
// Watch is MicroWizard with its two mesh hooks reported: mesh.js calls
// client() when a member with a pin turns up and removeClient() when it goes,
// which is the mesh as a consumer sees it - one JSON line per event on stdout.

import MicroWizard from 'microwizard';

export const BASE_PORT = Number(process.env.MWM_BASE_PORT || 38999);
export const TAG = process.env.MWM_TAG || 'mwmtest';
export const HOST = '127.0.0.1';
export const BASES = [`${HOST}:${BASE_PORT}`];

for (const forbidden of [39999, 39000, 39001]) {
  if (BASE_PORT === forbidden) {
    throw new Error(`MWM_BASE_PORT ${forbidden} belongs to a real mesh`);
  }
}

for (const key of Object.keys(process.env)) {
  if (key.startsWith('wzdc_')) delete process.env[key];
}

export function say (label, what) {
  process.stdout.write(JSON.stringify({ node: label, t: Date.now(), ...what }) + '\n');
}

export class Watch extends MicroWizard {
  constructor (label) {
    super();
    this.label = label;
  }

  async client (config) {
    if (config.type !== 'balance') {
      say(this.label, {
        ev: 'add',
        pin: config.pin,
        host: config.host,
        port: config.port,
        id: config.id,
        keys: Object.keys(config)
      });
    }
    return super.client(config);
  }

  removeClient (msg) {
    say(this.label, { ev: 'remove', pin: msg.config.pin, id: msg.config.id });
    return super.removeClient(msg);
  }
}

export async function start (label, mesh = {}, mc = new Watch(label)) {
  const options = {
    host: HOST,
    bases: BASES,
    tag: TAG,
    monitor: false,
    sneeze: { silent: true },
    discover: {
      rediscover: true,
      multicast: { active: false },
      guess: { active: false },
      custom: { active: false }
    },
    ...mesh
  };

  for (const base of options.bases) {
    if (!base.startsWith(`${HOST}:`)) throw new Error(`refusing base ${base}`);
  }

  mc.use('mesh', options);
  await new Promise((resolve) => mc.ready(resolve));
  say(label, { ev: 'ready', id: mc.id });
  return mc;
}

// SIGTERM goes through role:mc,cmd:close, as chairo's stop does in the
// gateway: offline (every call answered retry_later_err_overload), wait for
// what is running, half a second more, close. A second signal ends it now.
export function stopOn (mc, label) {
  let stopping = false;
  const stop = async () => {
    if (stopping) process.exit(0);
    stopping = true;
    say(label, { ev: 'stopping' });
    setTimeout(() => process.exit(0), 3000).unref();
    await mc.close().catch(() => {});
    process.exit(0);
  };
  process.on('SIGTERM', stop);
  process.on('SIGINT', stop);
}
