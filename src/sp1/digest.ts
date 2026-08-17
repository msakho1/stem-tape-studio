/**
 * SHA-256 over canonical bytes. Uses the platform WebCrypto digest in both the
 * browser and the test runner; no third-party hash implementation is added and
 * no bytes leave the process.
 */

export async function sha256Hex(parts: Uint8Array[]): Promise<string> {
  let total = 0;
  for (const p of parts) total += p.length;
  const all = new Uint8Array(total);
  let o = 0;
  for (const p of parts) {
    all.set(p, o);
    o += p.length;
  }
  const digest = await crypto.subtle.digest("SHA-256", all.buffer.slice(all.byteOffset, all.byteOffset + all.length) as ArrayBuffer);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
