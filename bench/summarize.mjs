// The median over rounds of bench/run.sh's lines, as a table.
//
//   node bench/summarize.mjs results.jsonl [more.jsonl ...]
import fs from 'node:fs';

const rows = process.argv.slice(2).flatMap((f) =>
  fs.readFileSync(f, 'utf8').split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l))
);
const median = (xs) => {
  const s = [...xs].sort((a, b) => a - b);
  return s.length % 2 ? s[s.length >> 1] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2;
};
const groups = new Map();
for (const r of rows) {
  const key = r.suite === 'local' ? `local ${r.test}` : `c=${r.c} ${r.pair} ${r.mode}`;
  if (!groups.has(key)) groups.set(key, []);
  groups.get(key).push(r);
}
for (const [key, rs] of groups) {
  const rps = rs.map((r) => r.rps);
  const extra = rs[0].suite === 'local'
    ? `avg ${median(rs.map((r) => r.avgNs)).toFixed(0)} ns`
    : `p50 ${median(rs.map((r) => r.p50us)).toFixed(0)} us  p99 ${median(rs.map((r) => r.p99us)).toFixed(0)} us  failures ${rs.reduce((a, r) => a + r.failures, 0)}`;
  console.log(`${key.padEnd(30)} ${String(Math.round(median(rps))).padStart(9)} rps  (${rps.join(' / ')})  ${extra}`);
}
