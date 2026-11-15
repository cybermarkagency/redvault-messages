// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The website without the messenger: the landing page, links meant for the
// apps, signing in (with two-step verification), the plan, the admin panel,
// community memberships, and what it never asks the server for.
import { render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App, { readStartup } from "../App.js";
import { APP_DOWNLOADS } from "../lib/downloads.js";
import { parseSiteLink } from "../lib/siteLinks.js";
import { addUser, COMMUNITY_ID, fake, installFakeSite, MFA_CODE, PREMIUM_PAYMENT_LINK, resetFakeSite, signInAs } from "./fakeSiteServer.js";

const SECRET = "xFRiZvwqdHTBk7LYxE7NnPOlBICPHKLZvdXzZs6Dkxs";

function openAt(address: string) {
  window.history.replaceState(null, "", address);
}

beforeEach(() => {
  resetFakeSite();
  installFakeSite();
  localStorage.clear();
  sessionStorage.clear();
  openAt("/");
});

afterEach(() => vi.unstubAllGlobals());

async function signIn(user: ReturnType<typeof userEvent.setup>, handle: string, password = "correct horse battery staple") {
  await user.type(screen.getByLabelText("Handle"), handle);
  await user.type(screen.getByLabelText("Password"), password);
  await user.click(screen.getByRole("button", { name: "Sign in" }));
}

/** Nothing the website sends is about messaging. */
function expectNoMessaging() {
  for (const { url } of fake.requests) {
    expect(url).not.toMatch(/\/(conversations|messages|e2e|v1\/files|backup|shares|live|events|calls|group-calls|stories|channels|vault|friends|presence)\b/);
    expect(url).not.toContain(SECRET);
  }
  for (const { body } of fake.requests) expect(body).not.toContain(SECRET);
}

describe("the landing page", () => {
  it("says messaging is in the apps, with the three downloads, and has no sign-up", async () => {
    render(<App />);
    expect(screen.getByRole("heading", { name: "RedVault Messages runs in the apps" })).toBeInTheDocument();
    for (const d of APP_DOWNLOADS) {
      const link = screen.getByRole("link", { name: new RegExp(d.label) });
      expect(link).toHaveAttribute("href", d.url);
    }
    expect(APP_DOWNLOADS.map((d) => d.url)).toEqual([
      "https://www.redvaultsystems.com/redvaultmessages/desktop/RedVault-Messages.apk",
      "https://www.redvaultsystems.com/redvaultmessages/desktop/RedVault-Messages-Setup.exe",
      "https://www.redvaultsystems.com/redvaultmessages/desktop/redvaultmessages-x86_64.AppImage",
    ]);
    expect(screen.getByRole("link", { name: "Terms of Service" })).toHaveAttribute("href", "terms.html");
    expect(screen.getByRole("link", { name: "Privacy Policy" })).toHaveAttribute("href", "privacy.html");
    expect(screen.getByRole("button", { name: "Source code" })).toBeInTheDocument();
    expect(screen.queryByRole("button", { name: /create account/i })).toBeNull();
    expect(screen.queryByText(/confirm password/i)).toBeNull();
    await waitFor(() => expect(fake.requests.some((r) => r.url.endsWith("/api/auth/me"))).toBe(true));
    expectNoMessaging();
  });

  it("opens the transparency feed without an account, and comes back", async () => {
    const user = userEvent.setup();
    render(<App />);
    await user.click(screen.getByRole("button", { name: "View the feed" }));
    expect(await screen.findByText("Live Encrypted Network")).toBeInTheDocument();
    expect(await screen.findByText(/No public transactions yet/)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Back" }));
    expect(screen.getByRole("heading", { name: "RedVault Messages runs in the apps" })).toBeInTheDocument();
  });
});

describe("without WebAssembly (Tor Browser at Safer or Safest)", () => {
  it("still shows the landing page and downloads; only signing in says why it can't", async () => {
    vi.stubGlobal("WebAssembly", undefined);
    const user = userEvent.setup();
    render(<App />);
    expect(screen.getAllByRole("link", { name: /Download for/ })).toHaveLength(3);
    await user.click(screen.getAllByRole("button", { name: "Sign in" })[0]);
    expect(screen.getByText("This browser can't run Redvault's encryption")).toBeInTheDocument();
    expect(screen.queryByLabelText("Password")).toBeNull();
  });
});

describe("links the apps make", () => {
  it.each([
    [`/?invite=AbCdEf123_-x#${SECRET}`, "a group invite"],
    [`/?join=L1#${SECRET}`, "a call link"],
    ["/?group-call=0b6f8f0e-1c2d-4e5f-8a9b-0c1d2e3f4a5b", "a group call"],
    ["/?start-group-call=g1&kind=video", "a group call"],
    [`/?channel=0b6f8f0e-1c2d-4e5f-8a9b-0c1d2e3f4a5b#${SECRET}`, "a channel"],
  ])("%s says to open it in the app, and its secret leaves the address bar", async (address, name) => {
    openAt(address);
    render(<App />);
    expect(screen.getByRole("heading", { name: "Open this in the RedVault Messages app" })).toBeInTheDocument();
    expect(screen.getByText(new RegExp(`This is ${name}\\.`))).toBeInTheDocument();
    expect(screen.getAllByRole("link", { name: /Download for/ })).toHaveLength(3);
    await waitFor(() => expect(window.location.search + window.location.hash).toBe(""));
    expect(screen.queryByRole("button", { name: "Buy a membership" })).toBeNull();
    expectNoMessaging();
  });

  it("a community link can also buy a membership here, after signing in", async () => {
    addUser({ username: "buyer" });
    openAt(`/?community=${COMMUNITY_ID}&invite=TOKEN12345abc`);
    const user = userEvent.setup();
    render(<App />);
    expect(screen.getByText(/This is a community\./)).toBeInTheDocument();
    await waitFor(() => expect(window.location.search).toBe(""));
    await user.click(screen.getByRole("button", { name: "Buy a membership" }));
    await signIn(user, "buyer");
    expect(await screen.findByRole("heading", { name: "Night Owls" })).toBeInTheDocument();
    const subscribe = screen.getByRole("button", { name: "Subscribe" });
    expect(subscribe).toBeDisabled();
    expect(fake.requests.some((r) => r.url.includes(`/api/communities/${COMMUNITY_ID}?invite=TOKEN12345abc`))).toBe(true);
    expectNoMessaging();
  });

  it("reads each kind of address", () => {
    expect(parseSiteLink("https://x.test/?invite=abc#s")).toEqual({ kind: "app", what: "group-invite" });
    expect(parseSiteLink(`https://x.test/?community=${COMMUNITY_ID}&invite=TOKEN12345abc`)).toEqual({
      kind: "app",
      what: "community",
      communityId: COMMUNITY_ID,
      inviteToken: "TOKEN12345abc",
    });
    expect(parseSiteLink(`https://x.test/?community-checkout=${COMMUNITY_ID}&result=success`)).toEqual({
      kind: "community-checkout",
      subscriptionId: COMMUNITY_ID,
      canceled: false,
    });
    expect(parseSiteLink("https://x.test/?community-memberships=1")).toEqual({ kind: "community-memberships" });
    expect(parseSiteLink("https://x.test/?creator-onboarding=done")).toEqual({ kind: "creator-onboarding" });
    expect(parseSiteLink("https://x.test/")).toBeNull();
    expect(readStartup("https://x.test/?purchase=p1&result=success").screen).toEqual({ key: "app-link", what: "payment" });
  });
});

describe("signing in", () => {
  it("a user without admin rights gets their plan, and signs out back to the landing page", async () => {
    addUser({ username: "alice", tier: "free" });
    const user = userEvent.setup();
    render(<App />);
    await user.click(screen.getAllByRole("button", { name: "Sign in" })[0]);
    await signIn(user, "alice");
    expect(await screen.findByRole("heading", { name: "Your plan" })).toBeInTheDocument();
    expect(await screen.findByRole("region", { name: "Free (your plan)" })).toBeInTheDocument();
    const premium = within(screen.getByRole("region", { name: "Premium" })).getByRole("link", { name: "Choose Premium" });
    const url = new URL(premium.getAttribute("href")!);
    expect(url.origin + url.pathname).toBe(PREMIUM_PAYMENT_LINK);
    expect(url.searchParams.get("client_reference_id")).toBe("user-1");
    expect(screen.queryByRole("button", { name: "Admin panel" })).toBeNull();
    expect(screen.getByRole("button", { name: "Community memberships" })).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: "Sign out" }));
    expect(await screen.findByRole("heading", { name: "RedVault Messages runs in the apps" })).toBeInTheDocument();
    expect(fake.session).toBeNull();
    expectNoMessaging();
  });

  it("a site admin gets the admin panel, and the plan too", async () => {
    addUser({ username: "root", role: "super_admin", tier: "premium" });
    addUser({ username: "bob" });
    const user = userEvent.setup();
    render(<App />);
    await user.click(screen.getAllByRole("button", { name: "Sign in" })[0]);
    await signIn(user, "root");
    expect(await screen.findByText("Admin panel", { selector: "div" })).toBeInTheDocument();
    expect(await screen.findByText("bob")).toBeInTheDocument();
    for (const tab of ["Users", "Reports", "Messages", "Communities"]) expect(screen.getByRole("button", { name: tab })).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Plan" }));
    expect(await screen.findByRole("region", { name: "Premium (your plan)" })).toBeInTheDocument();
    expectNoMessaging();
  });

  it("asks for the authenticator code when the account has one", async () => {
    addUser({ username: "carol", mfa: true });
    const user = userEvent.setup();
    render(<App />);
    await user.click(screen.getAllByRole("button", { name: "Sign in" })[0]);
    await signIn(user, "carol");
    await user.type(await screen.findByLabelText("Code"), "000000");
    await user.click(screen.getByRole("button", { name: "Verify" }));
    expect(await screen.findByText("That code isn't right.")).toBeInTheDocument();
    await user.clear(screen.getByLabelText("Code"));
    await user.type(screen.getByLabelText("Code"), MFA_CODE);
    await user.click(screen.getByRole("button", { name: "Verify" }));
    expect(await screen.findByRole("heading", { name: "Your plan" })).toBeInTheDocument();
  });

  it("says so when the handle or password is wrong", async () => {
    addUser({ username: "dave" });
    const user = userEvent.setup();
    render(<App />);
    await user.click(screen.getAllByRole("button", { name: "Sign in" })[0]);
    await signIn(user, "dave", "not the password");
    expect(await screen.findByText("Wrong handle or password.")).toBeInTheDocument();
  });

  it("sends the password only to sign-in, never to anything else", async () => {
    addUser({ username: "erin" });
    const user = userEvent.setup();
    render(<App />);
    await user.click(screen.getAllByRole("button", { name: "Sign in" })[0]);
    await signIn(user, "erin");
    await screen.findByRole("heading", { name: "Your plan" });
    const withPassword = fake.requests.filter((r) => r.body.includes("correct horse battery staple")).map((r) => r.url);
    expect(withPassword).toEqual(["/api/auth/login"]);
  });

  it("is still signed in after a reload (the session cookie)", async () => {
    signInAs(addUser({ username: "frank", tier: "pro" }));
    render(<App />);
    expect(await screen.findByRole("region", { name: "Pro (your plan)" })).toBeInTheDocument();
  });
});

describe("plans and payments", () => {
  it("moves a Pro subscriber to Premium after they confirm, and changes nothing when cancelled", async () => {
    signInAs(addUser({ username: "gina", tier: "pro" }));
    const user = userEvent.setup();
    render(<App />);
    await screen.findByRole("region", { name: "Pro (your plan)" });
    await user.click(await screen.findByRole("button", { name: "Switch to Premium…" }));
    await user.click(screen.getByRole("button", { name: "Cancel" }));
    expect(screen.getByRole("region", { name: "Pro (your plan)" })).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: "Switch to Premium…" }));
    expect(screen.getByText(/charged the difference for the rest of this billing period/i)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Switch to Premium" }));
    const premium = await screen.findByRole("region", { name: "Premium (your plan)" });
    expect(within(premium).getByText("Your plan · renews monthly")).toBeInTheDocument();
    expect(screen.getByText(/gina · Premium/)).toBeInTheDocument();
  });

  it("back from a community checkout: signs in, then confirms the membership with the server", async () => {
    addUser({ username: "hana" });
    openAt(`/?community-checkout=${COMMUNITY_ID}&result=success`);
    const user = userEvent.setup();
    render(<App />);
    await waitFor(() => expect(window.location.search).toBe(""));
    await signIn(user, "hana");
    expect(await screen.findByRole("heading", { name: "Community memberships" })).toBeInTheDocument();
    await waitFor(() => expect(fake.requests.some((r) => r.method === "POST" && r.url.endsWith(`/community-subscriptions/${COMMUNITY_ID}/refresh`))).toBe(true));
  });

  it("back from a canceled community checkout: says nothing was charged", async () => {
    signInAs(addUser({ username: "ivan" }));
    openAt(`/?community-checkout=${COMMUNITY_ID}&result=cancel`);
    render(<App />);
    expect(await screen.findByText("Checkout canceled. You weren't charged.")).toBeInTheDocument();
    expect(fake.requests.some((r) => r.url.includes("/refresh"))).toBe(false);
  });
});
