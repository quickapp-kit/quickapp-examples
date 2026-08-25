# V1 Showcase Suite

These are real alliance DSL applications built into deterministic RPK files.
They are showcase and integration baselines for the existing QuickApp Kit
runtime; they do not contain handwritten Page IR or runtime transactions.

| Case | Product surface | Build entry |
| --- | --- | --- |
| `gallery-001` | Device inspection and task board | `node gallery-001/scripts/build-gallery.mjs` |
| `consumer-001` | Daily content and task cards | `node consumer-001/scripts/build-consumer.mjs` |
| `wearable-001` | Compact wearable daily summary | `node wearable-001/scripts/build-wearable.mjs` |
| `commerce-001` | Android/iOS product-shaped C-end app | `node commerce-001/scripts/build-commerce.mjs` |
| `wallet-001` | Embedded constrained-screen card wallet | `node wallet-001/scripts/build-wallet.mjs` |

Each case has a Home/Detail route, state update, conditional rendering,
keyed list rendering, real `router.push()`/`router.back()`, small local PNG
assets, and a generated `dist/<case>.rpk` plus metadata file.
