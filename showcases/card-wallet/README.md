# Card Wallet

Card Wallet is the embedded constrained-screen showcase. It models a small
card wallet for door, transit and work cards; it does not implement NFC.

## Coverage

- 220x220 constrained viewport and compact safe layout
- Three keyed cards with Image/Text/Button
- Current-card state and `if` status
- Card Detail with real `router.push()` and `router.back()`
- Small local PNG resources under the embedded budget
- NFC is a later typed Feature; the first RPK remains deterministic and offline

## Build

```text
node scripts/build-card-wallet.mjs
```

The result is `dist/card-wallet.rpk` and metadata in `dist/card-wallet.json`.
