// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Where the apps are downloaded from: the fixed names on the website that
// always point at the newest release (see the downloads in deploy/).
const DOWNLOADS_BASE = "https://www.redvaultsystems.com/redvaultmessages/desktop/";

export interface AppDownload {
  platform: "Android" | "Windows" | "Linux";
  label: string;
  file: string;
  url: string;
}

export const APP_DOWNLOADS: AppDownload[] = [
  { platform: "Android", label: "Download for Android", file: "RedVault-Messages.apk" },
  { platform: "Windows", label: "Download for Windows", file: "RedVault-Messages-Setup.exe" },
  { platform: "Linux", label: "Download for Linux", file: "redvaultmessages-x86_64.AppImage" },
].map((d) => ({ ...d, platform: d.platform as AppDownload["platform"], url: DOWNLOADS_BASE + d.file }));
