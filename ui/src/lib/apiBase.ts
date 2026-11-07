// Where the backend lives — root-relative /api in dev, or a prefixed path
// (e.g. /redvaultmessages/api) when VITE_API_BASE is set at build time.
// See the comment at the top of api.ts.
// Defaults to "api" under wherever the app itself is served (Vite's base:
// "/" in dev and tests, "/redvaultmessages/" in a production build).
export const API_BASE: string = import.meta.env.VITE_API_BASE ?? `${import.meta.env.BASE_URL.replace(/\/$/, "")}/api`;

/**
 * The server hands back same-origin paths like "/api/users/<id>/avatar" (it
 * proxies avatars itself so no third-party host ever sees a viewer's IP).
 * Rebase them onto API_BASE so they also work under a path prefix.
 */
export function resolveApiUrl(url: string): string {
  return url.startsWith("/api/") ? `${API_BASE}${url.slice("/api".length)}` : url;
}
