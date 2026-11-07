// The shared encryption engine (crypto-core: vodozemac Olm and Megolm, device
// lists, pairing), compiled to WebAssembly by crypto-core/build-web.sh. It's
// optional: a build without crypto-core/pkg/web simply has no protocol-3
// preview, and the module is only downloaded when the preview is used.
//
// Every call is one JSON request and one JSON response. Requests carry the
// device's encrypted state and its storage key; responses carry the new
// encrypted state. Nothing here talks to the network.

export interface EngineResponse {
  ok: boolean;
  error?: string;
  message?: string;
  state?: string;
  [field: string]: unknown;
}

type Call = (request: string) => string;

interface EngineModule {
  default: (input?: unknown) => Promise<unknown>;
  call: Call;
}

const modules = import.meta.glob<EngineModule>("../../../../crypto-core/pkg/web/redvault_e2ee.js");

let loaded: Promise<Call | null> | null = null;

/** Whether this build includes the engine at all. */
export function engineIncluded(): boolean {
  return Object.keys(modules).length > 0 || loaded !== null;
}

export function loadEngine(): Promise<Call | null> {
  if (!loaded) {
    loaded = (async () => {
      const load = Object.values(modules)[0];
      if (!load) return null;
      const module = await load();
      await module.default();
      return module.call;
    })().catch(() => null);
  }
  return loaded;
}

/** Tests run the engine's Node build instead (see test/e2eV3Engine.ts). */
export function useEngineForTests(call: Call | null): void {
  loaded = Promise.resolve(call);
}

export class EngineUnavailableError extends Error {
  constructor() {
    super("The encryption engine isn't available in this build.");
  }
}

export async function runEngine(request: Record<string, unknown>): Promise<EngineResponse> {
  const call = await loadEngine();
  if (!call) throw new EngineUnavailableError();
  return JSON.parse(call(JSON.stringify(request))) as EngineResponse;
}
