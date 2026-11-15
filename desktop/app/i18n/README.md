# Translating the desktop app

Every piece of text the desktop app shows from its QML screens is wrapped in
`qsTr()`, so it can be translated with Qt's normal tools. No translation ships
yet: the app is English only.

To add a language (German, `de`, as the example):

1. Configure with the language listed: `cmake --preset release -DREDVAULT_LANGUAGES=de`.
2. Extract the strings: `cmake --build build/release --target update_translations`.
   This writes `app/i18n/redvaultmessages_de.ts`.
3. Translate that file in Qt Linguist (or any tool that reads `.ts`) and commit it.
4. Build as usual. The translation is compiled into the app, and the app uses it
   when the computer's language is German.

Have a native speaker check every translation before release: wording about
encryption, keys and payments must stay exact.

Not covered yet: messages that come from the C++ side (errors and notices built
in `src/` and `core/`) are still English; they need `tr()` in a later pass.
