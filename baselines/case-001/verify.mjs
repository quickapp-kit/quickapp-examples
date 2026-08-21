import { createHash } from 'node:crypto'
import { readdir, readFile, stat } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const baselineDir = dirname(fileURLToPath(import.meta.url))
const examplesDir = resolve(baselineDir, '../..')
const projectDir = resolve(examplesDir, 'quickapp-code-test1')

const comparePath = (left, right) => (left < right ? -1 : left > right ? 1 : 0)
const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex')
const readJson = async (name) => JSON.parse(await readFile(resolve(baselineDir, name), 'utf8'))

function invariant(condition, message) {
  if (!condition) throw new Error(message)
}

async function collectFiles(root, prefix = '') {
  const result = []
  for (const entry of await readdir(resolve(root, prefix), { withFileTypes: true })) {
    const relativePath = prefix ? `${prefix}/${entry.name}` : entry.name
    if (entry.isDirectory()) result.push(...await collectFiles(root, relativePath))
    else if (entry.isFile()) result.push(relativePath)
    else throw new Error(`Unsupported source entry: ${relativePath}`)
  }
  return result.sort(comparePath)
}

async function verifyFile(root, expected) {
  invariant(!expected.path.startsWith('/') && !expected.path.split('/').includes('..'), `Unsafe path: ${expected.path}`)
  const absolutePath = resolve(root, expected.path)
  const bytes = await readFile(absolutePath)
  invariant(bytes.byteLength === expected.byteLength, `Byte length mismatch: ${expected.path}`)
  invariant(sha256(bytes) === expected.sha256, `SHA-256 mismatch: ${expected.path}`)
}

function snapshotSha256(files) {
  const manifest = [...files]
    .sort((left, right) => comparePath(left.path, right.path))
    .map(({ path, sha256: digest }) => `${digest}  ${path}\n`)
    .join('')
  return sha256(Buffer.from(manifest, 'utf8'))
}

function readZipMembers(bytes) {
  const eocdSignature = 0x06054b50
  const centralSignature = 0x02014b50
  const minEocdOffset = Math.max(0, bytes.length - 65557)
  let eocdOffset = -1
  for (let offset = bytes.length - 22; offset >= minEocdOffset; offset -= 1) {
    if (bytes.readUInt32LE(offset) === eocdSignature) {
      eocdOffset = offset
      break
    }
  }
  invariant(eocdOffset >= 0, 'ZIP end-of-central-directory not found')
  const entryCount = bytes.readUInt16LE(eocdOffset + 10)
  let offset = bytes.readUInt32LE(eocdOffset + 16)
  const members = []
  for (let index = 0; index < entryCount; index += 1) {
    invariant(bytes.readUInt32LE(offset) === centralSignature, `Invalid ZIP central entry ${index}`)
    const byteLength = bytes.readUInt32LE(offset + 24)
    const nameLength = bytes.readUInt16LE(offset + 28)
    const extraLength = bytes.readUInt16LE(offset + 30)
    const commentLength = bytes.readUInt16LE(offset + 32)
    const path = bytes.subarray(offset + 46, offset + 46 + nameLength).toString('utf8')
    members.push({ path, byteLength })
    offset += 46 + nameLength + extraLength + commentLength
  }
  return members
}

function verifyExactPaths(actual, expected, label) {
  invariant(actual.length === expected.length, `${label} file count mismatch`)
  actual.forEach((path, index) => invariant(path === expected[index], `${label} path mismatch: ${path} != ${expected[index]}`))
}

async function verifySource() {
  const inventory = await readJson('source-inventory.json')
  invariant(inventory.caseId === 'CASE-001' && inventory.caseVersion === 1, 'Source Case identity mismatch')
  invariant(!inventory.files.some(({ path }) => /(^|\/)(sign|node_modules|build|dist)(\/|$)/.test(path)), 'Forbidden path entered Source inventory')
  const expectedPaths = inventory.files.map(({ path }) => path).sort(comparePath)
  const actualPaths = [...await collectFiles(resolve(projectDir, 'src'))].map((path) => `src/${path}`)
  actualPaths.push('package-lock.json', 'package.json')
  actualPaths.sort(comparePath)
  verifyExactPaths(actualPaths, expectedPaths, 'Source')
  await Promise.all(inventory.files.map((file) => verifyFile(projectDir, file)))
  invariant(snapshotSha256(inventory.files) === inventory.snapshotSha256, 'Source snapshot SHA-256 mismatch')
  return inventory
}

async function verifyReferences() {
  const inventory = await readJson('reference-inventory.json')
  for (const directory of inventory.directorySnapshots) {
    const actualPaths = (await collectFiles(resolve(projectDir, directory.path))).map((path) => `${directory.path}/${path}`)
    const expectedPaths = directory.files.map(({ path }) => path).sort(comparePath)
    verifyExactPaths(actualPaths, expectedPaths, directory.path)
    await Promise.all(directory.files.map((file) => verifyFile(projectDir, file)))
    invariant(snapshotSha256(directory.files) === directory.snapshotSha256, `Snapshot mismatch: ${directory.path}`)
  }
  for (const archive of inventory.archives) {
    await verifyFile(projectDir, archive)
    const actualMembers = readZipMembers(await readFile(resolve(projectDir, archive.path)))
    invariant(JSON.stringify(actualMembers) === JSON.stringify(archive.members), `ZIP member mismatch: ${archive.path}`)
  }
  return inventory
}

async function verifySemanticData(source) {
  const provenance = await readJson('provenance.json')
  const usage = await readJson('usage-matrix.json')
  const scenarios = await readJson('scenarios.json')
  for (const data of [provenance, usage, scenarios]) {
    invariant(data.caseId === source.caseId && data.caseVersion === source.caseVersion, 'Cross-file Case identity mismatch')
  }
  invariant(provenance.identity.sourceSnapshotSha256 === source.snapshotSha256, 'Provenance Source identity mismatch')
  invariant(Object.values(provenance.unknownOrigin).every(({ status, value }) => status === '[待验证]' && value === null), 'Unknown provenance must remain [待验证]')
  invariant(usage.outOfScope.some(({ feature }) => feature === 'system.device'), 'Usage matrix lost device exclusion')
  invariant(scenarios.sourceSnapshotSha256 === source.snapshotSha256, 'Scenario Source identity mismatch')
  invariant(scenarios.referencePackagesExecutableByRuntime === false, 'Alliance references cannot become Runtime input')
  invariant(scenarios.scenarios.map(({ id }) => id).join(',') === 'S1,S2,S3,S4,S5', 'Scenario sequence must be S1-S5')
  invariant(!JSON.stringify(scenarios).match(/screenX|screenY|nativeHandle/i), 'Scenario contains platform-specific locator')
}

const source = await verifySource()
await verifyReferences()
await verifySemanticData(source)
const sourceStats = await stat(projectDir)
invariant(sourceStats.isDirectory(), 'Case project directory is unavailable')
console.log(`CASE-001@1 baseline verified: ${source.snapshotSha256}`)
