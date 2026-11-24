import {mkdir, readFile, rename, writeFile} from 'node:fs/promises'
import {dirname, join} from 'node:path'
import {fileURLToPath} from 'node:url'
import {companionPath, remoteSources} from './content-sources.mjs'

const site = fileURLToPath(new URL('.', import.meta.url))
const repository = fileURLToPath(new URL('../../', import.meta.url))
const offline = process.env.HERMES_DOCS_OFFLINE === '1'

async function write(path, content) {
  await mkdir(dirname(path), {recursive: true})
  const temporary = `${path}.${process.pid}.tmp`
  await writeFile(temporary, content)
  await rename(temporary, path)
}

async function readSource(source, path) {
  const file = join(repository, 'target/docs-content/cache', source.id, source.revision, path)

  try {
    return await readFile(file, 'utf8')
  } catch (error) {
    if (error.code !== 'ENOENT') throw error
    if (offline) {
      throw Error(`Offline documentation cache is missing ${source.repository}@${source.revision}:${path}. Run npm run prepare:content with network access.`)
    }
  }

  const url = `https://raw.githubusercontent.com/${source.repository}/${source.revision}/${path}`
  const response = await fetch(url, {redirect: 'error', signal: AbortSignal.timeout(30_000)})
  if (!response.ok) throw Error(`Unable to fetch ${url} (HTTP ${response.status})`)
  const content = await response.text()
  if (!content.trim()) throw Error(`Empty documentation source at ${url}`)
  await write(file, content)
  return content
}

async function prepareSource(source) {
  if (!/^[a-f0-9]{40}$/.test(source.revision)) {
    throw Error(`Pin ${source.repository} to a full Git commit SHA.`)
  }
  if (source.paths.some(path => !/^(?:[A-Za-z0-9_-]+\/)*[A-Za-z0-9_.-]+\.md$/.test(path))) {
    throw Error(`Companion sources must list repository-relative Markdown files: ${source.repository}`)
  }

  const [license, ...pages] = await Promise.all(
    ['LICENSE', ...source.paths].map(path => readSource(source, path)),
  )

  for (const [index, content] of pages.entries()) {
    const path = source.paths[index]
    if (/^(?:\uFEFF)?---(?:\r?\n|$)/.test(content)) {
      throw Error(`Companion Markdown cannot set Docusaurus front matter: ${source.repository}:${path}`)
    }
    const sourceUrl = `https://github.com/${source.repository}/blob/${source.revision}/${path}`
    const attribution = `Source: [${source.title} at ${source.revision.slice(0, 12)}](${sourceUrl}) · [${source.license} license](pathname:///licenses/${source.id}.txt).`
    await write(join(repository, companionPath(source, path)), `${content.trimEnd()}\n\n---\n\n${attribution}\n`)
  }
  await write(join(site, '.generated/licenses', `${source.id}.txt`), license)
  console.log(`Prepared ${source.paths.length} pages from ${source.repository}@${source.revision.slice(0, 12)}`)
}

await Promise.all(remoteSources.map(prepareSource))
