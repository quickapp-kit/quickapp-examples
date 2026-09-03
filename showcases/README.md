# V1 Product Showcase Suite

These are user-facing product scenarios built from real alliance DSL into
deterministic RPK files. Narrow contract tests live under
`quickapp-examples/baseline-cases/`.

| Case | Product surface | Build entry |
| --- | --- | --- |
| `inspection-board` | Device inspection and task board | `node inspection-board/scripts/build-inspection-board.mjs` |
| `content-hub` | Daily content and task cards | `node content-hub/scripts/build-content-hub.mjs` |
| `health-summary` | Compact wearable daily summary | `node health-summary/scripts/build-health-summary.mjs` |
| `shop` | Android/iOS product-shaped C-end app | `node shop/scripts/build-shop.mjs` |
| `sport-watch` | Round smartwatch (240x240) fitness dashboard | `node sport-watch/scripts/build-sport-watch.mjs` |
| `sport-band` | Oval fitness band (194x368) vertical card flow | `node sport-band/scripts/build-sport-band.mjs` |
| `card-wallet` | Embedded constrained-screen card wallet | `node card-wallet/scripts/build-card-wallet.mjs` |

Each case has a Home/Detail route, state update, conditional rendering,
keyed list rendering, real `router.push()`/`router.back()`, small local PNG
assets, and a generated `dist/<case>.rpk` plus metadata file.
## Final V1 showcase set

- `showcases/shop/`: mobile product/content application with controlled Tabs.
- `showcases/sport-watch/`: round smartwatch fitness dashboard with multi-level navigation.
- `showcases/sport-band/`: oval fitness band vertical card flow with scrolling goal list.
- `showcases/card-wallet/`: constrained-screen card wallet.
- `showcases/inspection-board/`: embedded device inspection task board.
- `showcases/content-hub/`: compact consumer content-card application.
- `showcases/health-summary/`: compact wearable daily summary.
