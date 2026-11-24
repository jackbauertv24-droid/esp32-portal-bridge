// Usage: ./t_submit | node tests/verify_submit.mjs
// Checks each PDU from the C encoder with node-pdu: destination, text (parts
// rejoined in order) and part numbering.
import * as P from 'node-pdu';
const input = await new Promise(r => { let d = ''; process.stdin.on('data', c => d += c).on('end', () => r(d)); });
let fail = 0;
for (const c of JSON.parse(input)) {
  const parsed = c.parts.map(h => P.parse(h));
  const text = parsed.map(p => p.data.getText()).join('');
  const digits = c.to.replace(/\D/g, '');
  const addr = parsed[0].address.phone;
  const ok = text === c.text && addr === digits;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${c.parts.length} part(s) to ${c.to}: ${JSON.stringify(text).slice(0, 60)}`);
  if (!ok) { fail++; console.log('   want', JSON.stringify(c.text), 'addr', digits, 'got addr', addr); }
}
process.exit(fail ? 1 : 0);
