# Translating the Android app

The text on the app's screens lives in `app/src/main/res/values/strings_ui.xml`
(plus the app name in `strings.xml`), so it can be translated the standard
Android way. No translation ships yet: the app is English only.

To add a language (German, `de`, as the example):

1. Copy `app/src/main/res/values/strings_ui.xml` to `app/src/main/res/values-de/strings_ui.xml`.
2. Translate the text between the tags. Keep the `name` attributes, and keep `\'`, `\"` and `\n` escapes.
3. Build as usual. Android shows the German text when the phone's language is German,
   and falls back to English for anything not translated.

Have a native speaker check every translation before release: wording about
encryption, keys and payments must stay exact.

Not covered yet:
- text built with values (`"$count unread"`), about 50 places: these need format strings
  (`<string name="…">%1$d unread</string>`) in a later pass;
- messages created outside the screens (errors from `data/`, notifications, toasts).
