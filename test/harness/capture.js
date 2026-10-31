// Boots base, echo and a watching client with capture-hook.js loaded, lets
// them settle, kills echo hard so pings fail, ping-reqs go out and suspect
// and faulty updates travel, and writes everything to one JSONL file.
//
//   node capture.js [out.jsonl] [seconds-before-kill] [seconds-after-kill]
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const out = path.resolve(process.argv[2] || 'capture.jsonl');
const before = Number(process.argv[3] || 3) * 1000;
const after = Number(process.argv[4] || 5) * 1000;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

fs.writeFileSync(out, '');

const run = (script, label, ...args) => {
  const child = spawn(
    process.execPath,
    ['--import', path.join(here, 'capture-hook.js'), path.join(here, script), label, ...args],
    {
      env: { ...process.env, MWM_CAPTURE: out, MWM_CAPTURE_LABEL: label },
      stdio: ['ignore', 'inherit', 'inherit']
    }
  );
  return child;
};

const base = run('base.js', 'base');
await sleep(800);
const echo = run('echo.js', 'echo');
await sleep(400);
const client = run('client.js', 'client', '-', '{}', '--watch');
await sleep(before);
echo.kill('SIGKILL');
await sleep(after);
client.kill('SIGTERM');
base.kill('SIGTERM');
await sleep(300);

const lines = fs.readFileSync(out, 'utf8').trim().split('\n');
const types = {};
for (const line of lines) {
  const d = JSON.parse(line);
  const walk = (m) => (m.parts ? m.parts.forEach(walk) : (types[m.type] = (types[m.type] || 0) + 1));
  walk(d.msg);
}
console.error(`captured ${lines.length} datagrams into ${out}:`, JSON.stringify(types));
