# Shop

Shop is the Android/iOS C-end showcase baseline (formerly `commerce-001`). It is
intentionally a product-shaped QuickApp, not a full commerce implementation.

## Coverage

- Controlled Tabs with Home, Seed, Cart and Me content states
- Tabs `change({ index, value })` updates the selected binding and visible content
- Product Home list with local product photos, Image, Text, Button, state, `if` and keyed `for`
- Product Detail with real `router.push()` and `router.back()`
- Cart list with detail navigation, local total calculation and a payment demo button
- Me settings list with notification, address and service entries
- Seed content uses a local two-column card flow, local images, and the packaged `assets/videos/seed-demo.mp4`
- Three 48x48 local PNG assets and deterministic content
- No network, storage, real payment, account or model dependency

The seed video is a static local resource packaged into this RPK. The current
Android Video host can materialize and play it through the normal Runtime
mount path; other platform playback remains dependent on that platform's
Video resource adapter.

## Asset provenance

The compact product photos are derived from real product imagery used as local
demo assets: a desk lamp from pngimg.com, a backpack product image from The
North Face, and a thermos product image from Drakensberg. They are resized to
48x48 PNGs for the embedded resource budget; the RPK never performs network
access.

## Build

```text
node scripts/build-shop.mjs
```

The result is `dist/shop.rpk` and metadata in `dist/shop.json`.

The RPK contains only the Home and ProductDetail routes. The four primary
sections are state-driven content on Home; they do not create four parallel
navigation surfaces. The product photos are compact local assets, not logos.
