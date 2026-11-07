// Reading a Blob's bytes everywhere this app runs: Blob.arrayBuffer() and
// Blob.text() are missing in older Safari (and jsdom); FileReader isn't.

export function blobBytes(blob: Blob): Promise<Uint8Array> {
  if (typeof blob.arrayBuffer === "function") return blob.arrayBuffer().then((buffer) => new Uint8Array(buffer));
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error ?? new Error("Couldn't read the file"));
    reader.readAsArrayBuffer(blob);
  });
}

export async function blobText(blob: Blob): Promise<string> {
  return new TextDecoder().decode(await blobBytes(blob));
}
