# Commerce-001

Commerce-001 is the Android/iOS C-end showcase baseline. It is intentionally
a product-shaped QuickApp, not a full commerce implementation.

## Coverage

- Controlled Tabs with Home, Category, Cart and Me content states
- Tabs `change({ index, value })` updates the selected binding and visible content
- Product Home list with Image, Text, Button, state, `if` and keyed `for`
- Product Detail with real `router.push()` and `router.back()`
- Three small local PNG assets and deterministic content
- No network, storage, payment, account or model dependency

## Build

```text
node scripts/build-commerce.mjs
```

The result is `dist/commerce-001.rpk` and metadata in `dist/commerce-001.json`.

The RPK contains only the Home and ProductDetail routes. The four primary
sections are state-driven content on Home; they do not create four parallel
navigation surfaces.
