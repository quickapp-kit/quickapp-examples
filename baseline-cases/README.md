# V1 Baseline Cases

These are narrow, deterministic RPKs for validating one contract or runtime
capability at a time. User-facing product scenarios belong under `showcases/`.

| Case | Contract focus | Build entry |
| --- | --- | --- |
| `controls-001` | Input and Switch | `node scripts/build-controls.mjs` |
| `controls-002` | Slider and Picker | `node scripts/build-controls.mjs` |
| `list-001` | List and Scroll | `node scripts/build-list.mjs` |
| `long-list-001` | Long-list Mount capacity | build from its local fixture |
| `media-001` | Video and local media resource | `node scripts/build-media.mjs` |
| `platform-001` | prompt, fetch and file Features | `node scripts/build-platform.mjs` |
| `tabs-001` | Controlled Tabs and change | `node scripts/build-tabs.mjs` |
| `url-001` | Internal and external URL semantics | `node scripts/build-url.mjs` |
| `capability-gallery-001` | Final component and Feature acceptance index | `node scripts/build-capability-gallery.mjs` |

Every case is built from alliance DSL by Toolkit into `dist/<case>.rpk`.
