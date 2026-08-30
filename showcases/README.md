# V1 Showcase Suite

These are real alliance DSL applications built into deterministic RPK files.
They are showcase and integration baselines for the existing QuickApp Kit
runtime; they do not contain handwritten Page IR or runtime transactions.

| Case | Product surface | Build entry |
| --- | --- | --- |
| `gallery-001` | Device inspection and task board | `node gallery-001/scripts/build-gallery.mjs` |
| `consumer-001` | Daily content and task cards | `node consumer-001/scripts/build-consumer.mjs` |
| `wearable-001` | Compact wearable daily summary | `node wearable-001/scripts/build-wearable.mjs` |
| `shop` | Android/iOS product-shaped C-end app | `node shop/scripts/build-shop.mjs` |
| `sport-watch` | Round smartwatch (240x240) fitness dashboard | `node sport-watch/scripts/build-sport-watch.mjs` |
| `sport-band` | Oval fitness band (194x368) vertical card flow | `node sport-band/scripts/build-sport-band.mjs` |
| `wallet-001` | Embedded constrained-screen card wallet | `node wallet-001/scripts/build-wallet.mjs` |

Each case has a Home/Detail route, state update, conditional rendering,
keyed list rendering, real `router.push()`/`router.back()`, small local PNG
assets, and a generated `dist/<case>.rpk` plus metadata file.
## Final V1 showcase set

- `showcases/capability-gallery-001/`: independent component and Feature acceptance pages.
- `showcases/shop/`: mobile product/content application with controlled Tabs.
- `showcases/sport-watch/`: round smartwatch fitness dashboard with multi-level navigation.
- `showcases/sport-band/`: oval fitness band vertical card flow with scrolling goal list.
- `showcases/wearable-001/`: compact card-wallet style wearable application using Scroll/List.
