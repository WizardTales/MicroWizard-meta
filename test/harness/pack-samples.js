// What msgpackr 1.11.2's default pack - the one swim-ng's codec binds -
// writes for the values these messages can hold, edge by edge.
//
//   node pack-samples.js > ../fixtures/msgpackr-samples.txt
//
// <name> TAB <hex> TAB <typed dump>, the same shape as swim-bodies.txt. The
// meta encoder must produce these bytes from the same values, and its
// decoder the same dump from these bytes. Values JS cannot hold apart (5 and
// 5.0) are one value; the encoder follows JS there, see docs/protocol.md.
import { pack, unpack } from 'msgpackr';
import { dump } from './lib/mpdump.js';

const big = 'x'.repeat(40);
const samples = {
  'int-0': 0,
  'int-127': 127,
  'int-128': 128,
  'int-255': 255,
  'int-256': 256,
  'int-65535': 65535,
  'int-65536': 65536,
  'int-u32max': 0xffffffff,
  'int-u32max+1': 0x100000000,
  'int-neg1': -1,
  'int-neg32': -32,
  'int-neg33': -33,
  'int-neg128': -128,
  'int-neg129': -129,
  'int-neg32768': -32768,
  'int-neg32769': -32769,
  'int-i32min': -0x80000000,
  'int-i32min-1': -0x80000001,
  'date-now': 1793405011551,
  'date-now+1': 1793405011552,
  'max-safe': Number.MAX_SAFE_INTEGER,
  'float-1.5': 1.5,
  'float-neg0.1': -0.1,
  'neg-zero': -0,
  'bigint-u64': 18446744073709551615n,
  'bigint-i64': -9223372036854775808n,
  'bigint-small': 5n,
  'true': true,
  'false': false,
  'null': null,
  'undefined': undefined,
  'str-empty': '',
  'str-31': 'a'.repeat(31),
  'str-32': 'a'.repeat(32),
  'str-255': 'b'.repeat(255),
  'str-256': 'c'.repeat(256),
  'str-65536': 'd'.repeat(65536),
  'str-utf8': 'grüße ✓ 🧙',
  'str-escapes': 'q"b\\n\nt\tc\u0001',
  'arr-empty': [],
  'arr-15': Array.from({ length: 15 }, (_, i) => i),
  'arr-16': Array.from({ length: 16 }, (_, i) => i),
  'map-empty': {},
  'map-1': { seq: 0 },
  'map-16': Object.fromEntries(Array.from({ length: 16 }, (_, i) => [`k${i}`, i])),
  'map-nested': { a: { b: [1, { c: null }], d: big } },
  'bin-3': Buffer.from([0, 1, 255]),
  'bin-300': Buffer.alloc(300, 7),
  'ping': { seq: 0 },
  'ping-big-seq': { seq: 70000 },
  'pingreq': { seq: 9, destination: '127.0.0.1:45640' }
};

for (const [name, value] of Object.entries(samples)) {
  const bytes = pack(value);
  unpack(bytes); // must round trip through msgpackr itself
  process.stdout.write(`${name}\t${bytes.toString('hex')}\t${dump(bytes)}\n`);
}
