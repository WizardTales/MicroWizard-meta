// Turns a capture (capture.js) into the golden files the meta tests read.
//
//   node fixtures.js capture.jsonl ../fixtures
//
// swim-bodies.txt   one msgpack body per kind of message seen:
//                   <name> TAB <hex> TAB <typed dump>
// swim-frames.txt   whole datagrams, one per shape (single, compound):
//                   <name> TAB <hex> TAB <type>:<body hex> ...
//
// The dump is mpdump.js's, written from the bytes; the meta side decodes the
// hex, writes its own dump and must agree, and must encode its decoded value
// back to the same bytes.
import fs from 'node:fs';
import path from 'node:path';
import { unpack } from 'msgpackr';

const [input, dir] = process.argv.slice(2);
const bodies = new Map();
const frames = new Map();

const kind = (m) => {
  if (m.type !== 'Update' && m.type !== 'Sync') return m.type;
  const state = /"state":u:(\d)/.exec(m.dump)[1];
  const who = /"pin":\["([^"]+)"\]/.exec(m.dump);
  return `${m.type}-state${state}-${who ? who[1].split(',')[0].replace(':', '-') : 'nopin'}`;
};

for (const line of fs.readFileSync(input, 'utf8').trim().split('\n')) {
  const d = JSON.parse(line);
  const m = d.msg;
  if (m.error) throw new Error(`undecodable datagram ${d.hex}: ${m.error}`);
  const parts = m.parts || [m];
  for (const p of parts) {
    // msgpackr must agree with the hand reader about what the value is
    const v = unpack(Buffer.from(p.body, 'hex'));
    if (typeof v !== 'object') throw new Error(`body is not a map: ${p.body}`);
    const name = kind(p);
    if (!bodies.has(name)) bodies.set(name, p);
  }
  const shape = m.parts ? `compound${m.parts.length}-${m.parts.map((p) => p.type).join('+')}` : `single-${m.type}`;
  if (!frames.has(shape)) frames.set(shape, { hex: d.hex, parts });
}

fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(
  path.join(dir, 'swim-bodies.txt'),
  [...bodies].sort().map(([n, p]) => `${n}\t${p.body}\t${p.dump}`).join('\n') + '\n'
);
fs.writeFileSync(
  path.join(dir, 'swim-frames.txt'),
  [...frames].sort().map(([n, f]) => `${n}\t${f.hex}\t${f.parts.map((p) => `${p.type}:${p.body}`).join(' ')}`).join('\n') + '\n'
);
console.error(`${bodies.size} bodies, ${frames.size} frames into ${dir}`);
