// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useState } from "react";
import * as api from "../lib/api.js";
import { type OpenedCallLink, newCallLinkSecret, openOwnedLink, sealLinkName, sealSecretForOwner } from "../lib/callLinkCrypto.js";
import { ready } from "../lib/e2eCrypto.js";
import { useAppState } from "../state/AppStateContext.js";

/** Your call links, opened with your own keys (the server only has them sealed). */
export function useCallLinks() {
  const { state } = useAppState();
  const identity = state.identity;
  const [links, setLinks] = useState<OpenedCallLink[]>([]);
  const [loaded, setLoaded] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    if (!identity) return;
    try {
      await ready();
      const list = await api.listCallLinks();
      setLinks((list ?? []).map((l) => openOwnedLink(l, identity.publicKey, identity.privateKey)).filter((l): l is OpenedCallLink => l !== null));
      setError(null);
    } catch (err) {
      // An older server has no call links: show none rather than an error.
      if (!(err instanceof api.ApiError && (err.status === 404 || err.status === 503))) setError("Couldn't load your call links.");
    } finally {
      setLoaded(true);
    }
  }, [identity]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const create = useCallback(
    async (name: string, kind: api.CallKind): Promise<OpenedCallLink | null> => {
      if (!identity) return null;
      await ready();
      const secret = newCallLinkSecret();
      const dto = await api.createCallLink({ kind, secretSealed: sealSecretForOwner(secret, identity.publicKey), ...sealLinkName(name, secret) });
      await refresh();
      return openOwnedLink(dto, identity.publicKey, identity.privateKey);
    },
    [identity, refresh]
  );

  const remove = useCallback(
    async (id: string) => {
      await api.deleteCallLink(id);
      await refresh();
    },
    [refresh]
  );

  return { links, loaded, error, create, remove, refresh };
}
