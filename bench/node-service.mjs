// mw.js of WizardTales/microservice-benchmarks on a port of our choosing:
// a microwizard transport listener serving {cmd:'add'} -> {res: a + b}.
//
//   node node-service.mjs PORT
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const dir = process.env.MWB_DIR || path.resolve(path.dirname(new URL(import.meta.url).pathname), '../../microservice-benchmarks');
const require = createRequire(path.join(dir, 'index.js'));
const { default: Microwizard, transport } = await import(pathToFileURL(require.resolve('microwizard')));

process.setMaxListeners(0);
const mc1 = new Microwizard();
const tp = transport({ host: '127.0.0.1', port: Number(process.argv[2]) }, mc1);
mc1.add({ cmd: 'add' }, (msg) => ({ res: msg.a + msg.b }));
// 1.5.0's listener calls a third argument after every request (index.js
// passes () => this.reduceActive()); 1.1.1 ignores it
tp.listen({}, () => console.error('node-service listening', process.argv[2]),
  () => mc1.reduceActive?.());
