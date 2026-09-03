import { fileURLToPath } from 'node:url'
process.env.QUICKAPP_SHOWCASE_ROOT = fileURLToPath(new URL('..', import.meta.url))
await import('../../../scripts/build-showcase.mjs')
