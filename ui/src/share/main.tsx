// The share page's own entry (ui/share/index.html): a separate, small
// bundle with none of the app (no account, no chats, no state), served with
// its own stricter Content-Security-Policy. See docs/public-share-links.md.
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { SharePage } from "./SharePage.js";
import "../styles/global.css";

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <SharePage />
  </StrictMode>
);
