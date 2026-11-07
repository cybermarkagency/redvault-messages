// The "Open-Source Licenses and Source Code" section (Settings > About and the sign-in screens).
import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeEach, describe, expect, it, vi } from "vitest";
import App from "../App.js";
import { OpenSourceNotice } from "../components/OpenSourceNotice.js";
import { SourceCodeLink } from "../components/OpenSourceDialog.js";
import { resolveSourceOffer } from "../lib/sourceOffer.js";
import { AboutSettings } from "../screens/settings/GeneralSettings.js";
import { resetFakeBackend } from "./fakeApiBackend.js";

vi.mock("../lib/api.js", async () => {
  const backend = await import("./fakeApiBackend.js");
  return backend.fakeApiModule;
});

const LICENCE = "RedVault Messages is free software, licensed under the GNU Affero General Public License, version 3 only (AGPL-3.0-only). It comes with no warranty.";

describe("open-source notice", () => {
  it("with no source URL configured: says it isn't published, makes no source link, and falls back to the Terms contact", () => {
    render(<OpenSourceNotice offer={resolveSourceOffer({}, "9.8.7")} />);
    const section = screen.getByRole("region", { name: "Open-Source Licenses and Source Code" });
    expect(within(section).getByText(LICENCE)).toBeInTheDocument();
    expect(within(section).getByText("RedVault Messages web 9.8.7")).toBeInTheDocument();
    expect(within(section).getByText("The public source code link will appear here once it is published.")).toBeInTheDocument();
    expect(within(section).queryByRole("link", { name: /source code for this version/i })).toBeNull();
    expect(within(section).getByRole("link", { name: /full licence text/i })).toHaveAttribute("href", "/legal/LICENSE.txt");
    expect(within(section).getByRole("link", { name: /third-party notices/i })).toHaveAttribute("href", "/legal/THIRD_PARTY_NOTICES.txt");
    expect(within(section).getByRole("link", { name: "Terms of Service" })).toHaveAttribute("href", "terms.html");
    expect(within(section).getByText("RedVault Messages is not affiliated with or endorsed by Signal Messenger LLC.")).toBeInTheDocument();
  });

  it("with a source URL and contact configured: links both", () => {
    render(
      <OpenSourceNotice offer={resolveSourceOffer({ VITE_SOURCE_CODE_URL: "https://code.example.org/rv", VITE_SOURCE_CONTACT: "source@example.org" }, "1.0.0")} />,
    );
    expect(screen.getByRole("link", { name: "Source code for this version" })).toHaveAttribute("href", "https://code.example.org/rv");
    expect(screen.queryByText(/will appear here once it is published/)).toBeNull();
    expect(screen.getByRole("link", { name: "source@example.org" })).toHaveAttribute("href", "mailto:source@example.org");
  });

  it("the footer link goes straight to the source when set", () => {
    render(<SourceCodeLink offer={resolveSourceOffer({ VITE_SOURCE_CODE_URL: "https://code.example.org/rv" })} />);
    expect(screen.getByRole("link", { name: "Source code" })).toHaveAttribute("href", "https://code.example.org/rv");
  });

  it("is part of Settings > About", () => {
    render(<AboutSettings />);
    const section = screen.getByRole("region", { name: "Open-Source Licenses and Source Code" });
    expect(within(section).getByText(LICENCE)).toBeInTheDocument();
  });

  describe("in the app", () => {
    beforeEach(() => {
      resetFakeBackend();
      localStorage.clear();
    });

    it("is reachable from the sign-up screen without an account", async () => {
      const user = userEvent.setup();
      render(<App />);
      await user.click(screen.getByRole("button", { name: "Source code" }));
      const dialog = screen.getByRole("dialog", { name: "Open-Source Licenses and Source Code" });
      expect(within(dialog).getByText(LICENCE)).toBeInTheDocument();
      expect(within(dialog).getByText("The public source code link will appear here once it is published.")).toBeInTheDocument();
      await user.click(within(dialog).getByRole("button", { name: /^done$/i }));
      expect(screen.queryByRole("dialog")).toBeNull();
    });
  });
});
