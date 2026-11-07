import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import App from "./App.js";
import { WebAssemblyGate } from "./components/WebAssemblyGate.js";
import { SharedFile } from "./screens/SharedFile.js";
import "./styles/global.css";

// A share link to a file (…?file=<id>#<secret>) opens on its own: no sign-in, no app.
const sharedFileId = new URLSearchParams(window.location.search).get("file");

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <WebAssemblyGate>{sharedFileId ? <SharedFile linkId={sharedFileId} /> : <App />}</WebAssemblyGate>
  </StrictMode>
);
