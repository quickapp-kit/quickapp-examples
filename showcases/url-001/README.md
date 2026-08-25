# URL 001

This baseline verifies the three B6 URL semantics:

- `/pages/Detail` is lowered to an existing Button and enters Core Router.
- `open-mode="external"` calls `system.openUrl`.
- `open-mode="webview"` calls `system.webview`.

The URL feature is typed and platform-provided. This case does not embed a WebView Host Component or implement web-page scripting.
