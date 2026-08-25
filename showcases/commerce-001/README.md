# Commerce-001

Commerce-001 is the Android/iOS C-end showcase baseline. It is intentionally
a product-shaped QuickApp, not a full commerce implementation.

## Coverage

- Product Home list with Image, Text, Button, state, `if` and keyed `for`
- Product Detail with real `router.push()` and `router.back()`
- Bottom navigation: Home, Live, AI Agent and Me
- Small local PNG assets and deterministic content
- No network, storage, payment, account or model dependency

## Build

```text
node scripts/build-commerce.mjs
```

The result is `dist/commerce-001.rpk` and metadata in `dist/commerce-001.json`.
