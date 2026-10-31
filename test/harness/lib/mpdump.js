// A msgpack reader that keeps what the bytes said, not what JS made of them.
//
// msgpackr.unpack answers with JS values, which forgets exactly what this
// harness is for: whether Date.now() went out as a float64 or a uint64, and
// whether a map was a fixmap or a map16. This walks the bytes itself and
// writes the typed dump the meta side writes too (see docs/protocol.md,
// "Typisierte Darstellung"), so the two can be compared as text.
//
//   nil  true  false  u:5  i:-5  f:1730000000000  f:0x3ff8000000000000
//   "text"  bin:00ff  [a,b]  {"k":v}
//
// A fixext 1 of type 0 (msgpackr's undefined) is written nil, because that
// is all the meta side can make of it.

export function dump (buf) {
  const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let at = 0;

  const float = (v) =>
    Number.isInteger(v) && Math.abs(v) <= Number.MAX_SAFE_INTEGER
      ? `f:${v}`
      : (() => {
          const b = new DataView(new ArrayBuffer(8));
          b.setFloat64(0, v);
          return 'f:0x' + b.getBigUint64(0).toString(16).padStart(16, '0');
        })();

  const int = (v) => (v >= 0n || v >= 0 ? `u:${v}` : `i:${v}`);

  const str = (n) => {
    const s = buf.subarray(at, at + n).toString('utf8');
    at += n;
    return JSON.stringify(s);
  };

  const bin = (n) => {
    const s = buf.subarray(at, at + n).toString('hex');
    at += n;
    return `bin:${s}`;
  };

  const arr = (n) => {
    const out = [];
    for (let i = 0; i < n; ++i) out.push(value());
    return `[${out.join(',')}]`;
  };

  const map = (n) => {
    const out = [];
    for (let i = 0; i < n; ++i) {
      const k = value();
      out.push(`${k}:${value()}`);
    }
    return `{${out.join(',')}}`;
  };

  const ext = (n) => {
    const type = view.getInt8(at);
    at += 1;
    const data = buf.subarray(at, at + n);
    at += n;
    if (type === 0 && n === 1 && data[0] === 0) return 'nil';
    return `ext:${type}:${data.toString('hex')}`;
  };

  function value () {
    const b = buf[at++];
    if (b === undefined) throw new Error('truncated');
    if (b <= 0x7f) return `u:${b}`;
    if (b >= 0xe0) return `i:${b - 0x100}`;
    if ((b & 0xf0) === 0x80) return map(b & 0x0f);
    if ((b & 0xf0) === 0x90) return arr(b & 0x0f);
    if ((b & 0xe0) === 0xa0) return str(b & 0x1f);
    let v;
    switch (b) {
      case 0xc0: return 'nil';
      case 0xc2: return 'false';
      case 0xc3: return 'true';
      case 0xc4: v = buf[at]; at += 1; return bin(v);
      case 0xc5: v = view.getUint16(at); at += 2; return bin(v);
      case 0xc6: v = view.getUint32(at); at += 4; return bin(v);
      case 0xc7: v = buf[at]; at += 1; return ext(v);
      case 0xc8: v = view.getUint16(at); at += 2; return ext(v);
      case 0xc9: v = view.getUint32(at); at += 4; return ext(v);
      case 0xca: v = view.getFloat32(at); at += 4; return float(v);
      case 0xcb: v = view.getFloat64(at); at += 8; return float(v);
      case 0xcc: v = buf[at]; at += 1; return int(v);
      case 0xcd: v = view.getUint16(at); at += 2; return int(v);
      case 0xce: v = view.getUint32(at); at += 4; return int(v);
      case 0xcf: v = view.getBigUint64(at); at += 8; return int(v);
      case 0xd0: v = view.getInt8(at); at += 1; return int(v);
      case 0xd1: v = view.getInt16(at); at += 2; return int(v);
      case 0xd2: v = view.getInt32(at); at += 4; return int(v);
      case 0xd3: v = view.getBigInt64(at); at += 8; return int(v);
      case 0xd4: return ext(1);
      case 0xd5: return ext(2);
      case 0xd6: return ext(4);
      case 0xd7: return ext(8);
      case 0xd8: return ext(16);
      case 0xd9: v = buf[at]; at += 1; return str(v);
      case 0xda: v = view.getUint16(at); at += 2; return str(v);
      case 0xdb: v = view.getUint32(at); at += 4; return str(v);
      case 0xdc: v = view.getUint16(at); at += 2; return arr(v);
      case 0xdd: v = view.getUint32(at); at += 4; return arr(v);
      case 0xde: v = view.getUint16(at); at += 2; return map(v);
      case 0xdf: v = view.getUint32(at); at += 4; return map(v);
      default: throw new Error(`format 0x${b.toString(16)} is not msgpack`);
    }
  }

  const out = value();
  if (at !== buf.length) throw new Error(`${buf.length - at} bytes left over`);
  return out;
}

// The SWIM framing around the bodies: 1 byte type, then the body; a compound
// (type 0) is a u16le count and u16le-length-prefixed messages.
export const TYPES = {
  0: 'Compound',
  1: 'Compressed',
  2: 'Encrypted',
  10: 'Ping',
  11: 'PingReq',
  12: 'Sync',
  13: 'Ack',
  14: 'Update'
};

export function datagram (buf) {
  const type = buf[0];
  if (type === 0) {
    const count = buf.readUInt16LE(1);
    let at = 3;
    const parts = [];
    for (let i = 0; i < count; ++i) {
      const len = buf.readUInt16LE(at);
      at += 2;
      parts.push(datagram(buf.subarray(at, at + len)));
      at += len;
    }
    return { type: 'Compound', parts };
  }
  const body = buf.subarray(1);
  return { type: TYPES[type] || String(type), body: body.toString('hex'), dump: dump(body) };
}
