# Wallet-001

Wallet-001 is the embedded constrained-screen showcase. It models a small
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
node scripts/build-wallet.mjs
```

The result is `dist/wallet-001.rpk` and metadata in `dist/wallet-001.json`.
