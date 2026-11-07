# Play Console answers — Redvault Messages

Answers for the Play Console forms, based on what the app and server actually do.
Re-check them if the app starts collecting anything new.

## App content → Privacy policy

`https://www.redvaultsystems.com/redvaultmessages/privacy.html`

## App content → Data safety

**Does your app collect or share any of the required user data types?** Yes

**Is all of the user data collected by your app encrypted in transit?** Yes (HTTPS only)

**Do you provide a way for users to request that their data is deleted?** Yes
Delete-account URL: `https://www.redvaultsystems.com/redvaultmessages/privacy.html#delete-account`

### Data types

End-to-end encrypted data that you can't read doesn't count as "collected" in Play's
definition. That covers messages, files, voice messages, reactions and group names,
so leave those unticked.

| Category → type | Collected | Shared | Processed ephemerally | Required / optional | Purposes |
|---|---|---|---|---|---|
| Personal info → **User IDs** (username, account id) | Yes | No | No | Required | App functionality, Account management |
| Photos and videos → **Photos** (profile photo) | Yes | No | No | Optional | App functionality |
| App activity → **Other user-generated content** (abuse reports and their reason text) | Yes | No | No | Optional | Fraud prevention, security, and compliance |
| Device or other IDs → **Device or other IDs** (Firebase notification token) | Yes | No | No | Optional (notifications can be turned off) | App functionality |

Leave everything else unticked:
- Location, phone, email, name, address: none collected.
- Financial info: none in the app, because payment happens on the website through Stripe.
- Health, contacts, calendar, web history, installed apps: none.
- Messages, audio, files and docs: end-to-end encrypted, as explained above.
- App info and performance (crash logs, diagnostics): there's no crash or analytics SDK.

**Shared:** No for all. Firebase and Backblaze act as service providers for you, and a
photo a user chooses to show other users isn't "sharing" in Play's definition.

### Security practices section
- Data is encrypted in transit: **Yes**
- Users can request data deletion: **Yes**
- (Optional badge) Independent security review: **No** (unless you get one)

## App content → other forms

- **Ads:** No, the app contains no ads.
- **App access:** "All or some functionality is restricted." Give reviewers a test
  account's username and password (make a fresh one, e.g. `play-reviewer`). Don't turn on
  2FA or app lock for it. Say that messages go to friends, so they can add another test
  account as a friend, or give them two accounts.
- **Content rating:** answer the questionnaire as a communication app. Users can exchange
  messages and media with each other, and there's no moderation of message content because
  it's end-to-end encrypted. Users can block and report. Expect a rating like Teen or 12+.
- **Target audience:** choose **16–17 and 18+** (or 18+ only). Don't tick any age group under 13, because the app isn't for children.
- **News app:** No.
- **Government app:** No.
- **Financial features:** None.
- **Health:** None.
- **Data safety:** see above.
- **Account deletion:** Yes, in the app (Settings → Delete account) and on the web (URL above).

## Store listing (drafts, edit freely)

**App name:** Redvault Messages

**Short description (max 80 chars):**
End-to-end encrypted messenger. No phone number, no email, no tracking.

**Full description:**
Redvault Messages is a private messenger built to know as little about you as possible.

• End-to-end encrypted chats, group chats, files and voice messages. The server can't read them.
• Sign up with just a username and password. No phone number or email.
• Verify contacts with a safety number or QR code.
• Disappearing messages, replies, reactions, and delete for everyone.
• App lock with your own PIN, and a recovery key so a forgotten password isn't the end.
• Maximum Privacy Mode: hide read receipts, block screenshots, auto-lock with a PIN. Notifications never show who a message is from or what it says.
• No ads, no analytics, no trackers. IP addresses aren't stored.
• A public transparency feed lets you check that your messages were relayed untouched.

**Category:** Communication
**Contact email:** a mailbox you check (the privacy page uses privacy@redvaultsystems.com)
**Graphics you still need:** 1024×500 feature graphic and at least 2 phone screenshots.
The 512×512 icon is `android/store/play-store-icon-512.png`.
