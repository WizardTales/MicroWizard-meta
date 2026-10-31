// Records every UDP datagram this process sends or receives, decoded.
//
//   MWM_CAPTURE=/path/out.jsonl node --import ./capture-hook.js base.js
//
// Loaded before the node, it wraps dgram's send and the 'message' event, so
// what lands in the file are the bytes swim-ng really put on the wire - not a
// re-encoding of what it meant to send. One JSON line per datagram:
//
//   {t, dir: "out"|"in", local, peer, hex, msg: {type, parts|body, dump}}
//
// capture.js boots a small mesh with this loaded; fixtures.js turns the
// result into test/fixtures/.
import dgram from 'node:dgram';
import fs from 'node:fs';
import { datagram } from './lib/mpdump.js';

const out = process.env.MWM_CAPTURE;
const label = process.env.MWM_CAPTURE_LABEL || process.argv[2] || 'node';

if (out) {
  const record = (dir, socket, peer, buf) => {
    let local = null;
    try {
      local = socket.address().port;
    } catch (e) {}
    let msg;
    try {
      msg = datagram(Buffer.from(buf));
    } catch (e) {
      msg = { error: e.message };
    }
    fs.appendFileSync(
      out,
      JSON.stringify({
        t: Date.now(),
        node: label,
        dir,
        local,
        peer,
        hex: Buffer.from(buf).toString('hex'),
        msg
      }) + '\n'
    );
  };

  const send = dgram.Socket.prototype.send;
  dgram.Socket.prototype.send = function (buf, offset, length, port, address) {
    if (Buffer.isBuffer(buf) && typeof offset === 'number' && typeof length === 'number') {
      record('out', this, `${address}:${port}`, buf.subarray(offset, offset + length));
    } else if (Buffer.isBuffer(buf)) {
      record('out', this, `${arguments[2]}:${arguments[1]}`, buf);
    }
    return send.apply(this, arguments);
  };

  const emit = dgram.Socket.prototype.emit;
  dgram.Socket.prototype.emit = function (event, buf, rinfo) {
    if (event === 'message') {
      record('in', this, `${rinfo.address}:${rinfo.port}`, buf);
    }
    return emit.apply(this, arguments);
  };
}
