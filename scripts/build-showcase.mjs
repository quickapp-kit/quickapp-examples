import { createHash } from 'node:crypto'
import { mkdir, readFile, writeFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { Ajv2020 } from '../../quickapp-toolkit/node_modules/ajv/dist/2020.js'
import { CancellationController } from '../../quickapp-toolkit/dist/application/cancellation.js'
import { RuntimeArtifactBuilder } from '../../quickapp-toolkit/dist/compiler/artifact/runtime-artifact-builder.js'
import { ArtifactPaths } from '../../quickapp-toolkit/dist/compiler/artifact-paths.js'
import { JsModuleEmitter } from '../../quickapp-toolkit/dist/compiler/emitter/js-module-emitter.js'
import { PageIrEmitter } from '../../quickapp-toolkit/dist/compiler/emitter/page-ir-emitter.js'
import { CanonicalLowerer } from '../../quickapp-toolkit/dist/compiler/lowering/canonical-lowerer.js'
import { ModuleGraphBuilder } from '../../quickapp-toolkit/dist/compiler/module-graph/module-graph-builder.js'
import { SourceFrontend } from '../../quickapp-toolkit/dist/compiler/frontend/source-frontend.js'
import { SourceAccess } from '../../quickapp-toolkit/dist/workspace/source-access.js'

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url))
const showcaseRoot = path.resolve(process.env.QUICKAPP_SHOWCASE_ROOT ?? path.join(scriptDirectory, process.argv[2] ?? ''))
const caseName = path.basename(showcaseRoot)
const repositoryRoot = path.resolve(showcaseRoot, '../../..')
const schemaRoot = path.join(
  repositoryRoot,
  '../BBQ/docs/interview/BT/proj/quickapp-kit/v3/spec/contracts/schemas',
)
const outputRoot = path.join(showcaseRoot, 'dist')
const frameworkBundlePath = path.join(repositoryRoot, 'quickapp-runtime-js/dist/quickapp-framework-v1.js')
const frameworkMode = process.env.QUICKAPP_FRAMEWORK_MODE ?? 'shared'

const cancellation = () => new CancellationController().token
const sha256 = bytes => createHash('sha256').update(Buffer.from(bytes)).digest('hex')
const readJson = async file => JSON.parse(await readFile(file, 'utf8'))
const repositoryRelative = file => path.relative(repositoryRoot, file).split(path.sep).join('/')

async function schemaValidator() {
  const ajv = new Ajv2020({ allErrors: true, strict: true })
  const manifest = ajv.compile(await readJson(path.join(schemaRoot, 'manifest.schema.json')))
  const runtime = ajv.compile(await readJson(path.join(schemaRoot, 'runtime-metadata.schema.json')))
  const host = await readJson(path.join(schemaRoot, 'host-component.schema.json'))
  const pageAjv = new Ajv2020({ allErrors: true, strict: true })
  pageAjv.addSchema(host)
  const pageValidator = pageAjv.compile(await readJson(path.join(schemaRoot, 'page-ir.schema.json')))
  const errors = validate => value => validate(value) ? [] : (validate.errors ?? []).map(error => `${error.instancePath} ${error.message ?? 'invalid'}`)
  return {
    artifact: { validateManifest: errors(manifest), validateRuntimeMetadata: errors(runtime) },
    page: { validate: errors(pageValidator) },
    publicManifest: { validate: errors(manifest) },
  }
}

function mediaType(kind) {
  return kind === 'png' ? 'image/png' : kind === 'jpeg' || kind === 'jpg' ? 'image/jpeg' : 'application/octet-stream'
}

function pngSize(bytes) {
  const buffer = Buffer.from(bytes)
  if (buffer.length < 24 || buffer.toString('ascii', 1, 4) !== 'PNG') return { width: null, height: null }
  return { width: buffer.readUInt32BE(16), height: buffer.readUInt32BE(20) }
}

async function main() {
  const access = new SourceAccess(showcaseRoot, [])
  try {
    const manifestSource = await access.read('src/manifest.json', { content: 'strictUtf8', maxBytes: 2_000_000 })
    const validators = await schemaValidator()
    const graph = await new ModuleGraphBuilder().build({
      manifest: manifestSource,
      sourceRoot: 'src',
      sourceAccess: access,
      frontend: new SourceFrontend(),
      schemaValidator: validators.publicManifest,
      cancellation: cancellation(),
    })
    if (graph.status !== 'success') throw new Error(graph.diagnostics.map(diagnostic => diagnostic.message).join('; '))
    const lowered = new CanonicalLowerer().lower({
      resolvedAppModel: graph.model,
      parsedSourceModel: graph.parsedSources,
      cancellation: cancellation(),
    })
    if (lowered.status !== 'success') throw new Error(lowered.diagnostics.map(diagnostic => diagnostic.message).join('; '))
    const frameworkContent = frameworkMode === 'shared' ? await readFile(frameworkBundlePath, 'utf8') : undefined
    const framework = frameworkContent === undefined ? undefined : {
      moduleId: '@quickapp-kit/framework-v1',
      content: frameworkContent,
      sha256: sha256(Buffer.from(frameworkContent)),
      mode: 'shared',
    }
    const js = new JsModuleEmitter().emit({ model: lowered.model, ...(framework === undefined ? {} : { framework }), cancellation: cancellation() })
    if (js.status !== 'success') throw new Error(js.diagnostics.map(diagnostic => diagnostic.message).join('; '))
    const pageIr = new PageIrEmitter().emit({ model: lowered.model, schemaValidator: validators.page, cancellation: cancellation() })
    if (pageIr.status !== 'success') throw new Error(pageIr.diagnostics.map(diagnostic => diagnostic.message).join('; '))
    const resources = []
    const resourcePaths = new Set()
    for (const asset of graph.model.assets) {
      const source = await access.read(asset.sourcePath, { content: 'bytes', maxBytes: 16 * 1024 * 1024 })
      const resourcePath = asset.sourcePath.replace(/^src\//, '')
      resourcePaths.add(resourcePath)
      resources.push(Object.freeze({
        path: resourcePath,
        mediaType: mediaType(asset.mediaKind),
        bytes: Object.freeze(Array.from(source.bytes)),
      }))
    }
    const imageEntries = await access.list('src/assets/images', { maxEntries: 32 })
    for (const entry of imageEntries) {
      if (entry.kind !== 'file') continue
      const resourcePath = entry.logicalPath.replace(/^src\//, '')
      if (resourcePaths.has(resourcePath)) continue
      const source = await access.read(entry.logicalPath, { content: 'bytes', maxBytes: 16 * 1024 * 1024 })
      const extension = path.extname(entry.logicalPath).toLowerCase()
      if (!['.png', '.jpg', '.jpeg'].includes(extension)) continue
      resourcePaths.add(resourcePath)
      resources.push(Object.freeze({
        path: resourcePath,
        mediaType: extension === '.png' ? 'image/png' : 'image/jpeg',
        bytes: Object.freeze(Array.from(source.bytes)),
      }))
    }
    let videoEntries = []
    try {
      videoEntries = await access.list('src/assets/videos', { maxEntries: 8 })
    } catch (error) {
      if (!String(error).includes('Source not found')) throw error
    }
    for (const entry of videoEntries) {
      if (entry.kind !== 'file') continue
      const resourcePath = entry.logicalPath.replace(/^src\//, '')
      if (resourcePaths.has(resourcePath)) continue
      const extension = path.extname(entry.logicalPath).toLowerCase()
      if (extension !== '.mp4') continue
      const source = await access.read(entry.logicalPath, { content: 'bytes', maxBytes: 16 * 1024 * 1024 })
      resourcePaths.add(resourcePath)
      resources.push(Object.freeze({
        path: resourcePath,
        mediaType: 'video/mp4',
        resourceId: resourcePath,
        bytes: Object.freeze(Array.from(source.bytes)),
      }))
    }
    const artifact = new RuntimeArtifactBuilder().build({
      model: lowered.model,
      manifest: graph.model.manifest,
      js,
      pageIr,
      resources: Object.freeze(resources),
      toolkitVersion: '0.1.0',
      buildMode: 'debug',
      ...(framework === undefined ? {} : { framework: { moduleId: framework.moduleId, path: ArtifactPaths.frameworkBundle(framework.moduleId), content: frameworkContent, sha256: framework.sha256 } }),
      schemaValidator: validators.artifact,
      cancellation: cancellation(),
    })
    if (artifact.status !== 'success') throw new Error(artifact.diagnostics.map(diagnostic => diagnostic.message).join('; '))
    const imageMembers = []
    for (const resource of artifact.metadata.resources) {
      if (resource.mediaType !== 'image/png') continue
      const source = resources.find(item => item.path === resource.path)
      const dimensions = pngSize(source?.bytes ?? [])
      if (dimensions.width === null || dimensions.width > 48 || dimensions.height > 48 || resource.byteLength > 4096) {
        throw new Error(`image budget exceeded: ${resource.path}`)
      }
      imageMembers.push({
        path: resource.path,
        width: dimensions.width,
        height: dimensions.height,
        byteLength: resource.byteLength,
        sha256: resource.sha256,
      })
    }
    const imageTotalBytes = imageMembers.reduce((total, image) => total + image.byteLength, 0)
    if (imageTotalBytes > 12 * 1024) throw new Error('image total budget exceeded')
    await mkdir(outputRoot, { recursive: true })
    const packagePath = path.join(outputRoot, `${caseName}.rpk`)
    const metadataPath = path.join(outputRoot, `${caseName}.json`)
    await writeFile(packagePath, Buffer.from(artifact.packageBytes))
    const metadata = {
      status: 'PASS',
      case: caseName,
      sourceManifest: repositoryRelative(path.join(showcaseRoot, 'src/manifest.json')),
      packagePath: repositoryRelative(packagePath),
      packageByteLength: artifact.packageBytes.length,
      packageSha256: sha256(artifact.packageBytes),
      entryRoute: artifact.metadata.entryRoute,
      routes: artifact.metadata.pages.map(page => page.manifestRoute),
      imageMembers,
      imageTotalBytes,
      videoMembers: artifact.metadata.resources.filter(resource => resource.path.startsWith('assets/videos/')),
      deterministicBuild: true,
      requiredRuntimeOperations: ['updateBinding', 'removeBlock', 'moveBlock', 'navigationPush', 'navigationClose'],
      capabilities: graph.model.manifest.raw.features.map(feature => feature.name),
    }
    await writeFile(metadataPath, `${JSON.stringify(metadata, null, 2)}\n`)
    console.log(JSON.stringify(metadata, null, 2))
  } finally {
    access.dispose()
  }
}

await main()
