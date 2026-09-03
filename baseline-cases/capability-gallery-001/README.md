# Capability Gallery 001

This is the final V1 acceptance entry for QuickApp Kit. Home is a keyed
capability list; every item opens its own page and returns through the shared
router. Component pages cover View, Text, Button, Image, Input, Switch,
Slider, Picker, List, Scroll and Tabs. Feature pages use the real typed
facades for prompt, fetch, file and device. OpenUrl and Webview are not part
of this cross-platform RPK because their platform adapters are not in the
current common runtime capability set.

All pages use the Alliance DSL and are packaged by the existing Toolkit. The
only local image is a 32x32 PNG. No page writes UI, routing or Bridge logic in
the platform composition root.

Build:

```text
node scripts/build-capability-gallery.mjs
```
