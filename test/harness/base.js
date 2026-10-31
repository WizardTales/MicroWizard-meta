// The base node, as a microwizard base starts it: isbase, pin role:mesh,base:true,
// on the fixed SWIM port every other node is told about.
import { start, stopOn, BASE_PORT } from './lib/node.js';

const label = process.argv[2] || 'base';
const mc = await start(label, { isbase: true, base: true, port: BASE_PORT });
stopOn(mc, label);
