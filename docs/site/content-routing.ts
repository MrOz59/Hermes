import {execFileSync} from 'node:child_process'
import {existsSync} from 'node:fs'
import path from 'node:path'
import {fileURLToPath} from 'node:url'

import type {ParseFrontMatter} from '@docusaurus/types'

import {companionPath, companionRoot, remoteSources} from './content-sources.mjs'

const repositoryRoot = fileURLToPath(new URL('../../', import.meta.url))
const defaultRepository = 'MrOz59/Hermes'
const markdown = /\.(?:md|mdx)$/i
const packageDocument = /^(?:rust|mcpp)\/(?:.*\/)?(?:README\.md|docs\/.+\.mdx?)$/i

function localGit(args: string[]) {
  try {
    return execFileSync('git', args, {cwd: repositoryRoot, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore']}).trim()
  } catch {
    return undefined
  }
}

const gitHubOrigin = localGit(['remote', 'get-url', 'origin'])?.match(
  /^(?:https:\/\/github\.com\/|git@github\.com:)([^/]+\/[^/]+?)(?:\.git)?$/,
)?.[1]
const hostRepository = process.env.GITHUB_REPOSITORY ?? gitHubOrigin ?? defaultRepository
const hostRevision = process.env.GITHUB_SHA ?? localGit(['rev-parse', 'HEAD']) ?? 'main'

type Source = {
  id: string
  repository: string
  revision: string
  paths: readonly string[]
}

type MarkdownNode = {
  type: string
  url?: string
  identifier?: string
  title?: string | null
  children?: MarkdownNode[]
}

function companionFromPath(repositoryPath: string) {
  return (remoteSources as readonly Source[]).find(
    ({id}) => repositoryPath.startsWith(`${companionRoot}/${id}/`),
  )
}

function routeForDocument(repositoryPath: string) {
  if (repositoryPath.startsWith('docs/')) return repositoryPath.slice('docs/'.length)
  if (packageDocument.test(repositoryPath)) return `developers/${repositoryPath}`
  const companion = companionFromPath(repositoryPath)
  if (!companion) return undefined
  const file = repositoryPath.slice(companionPath(companion, '').length)
  return `companions/${companion.id}/${file.replace(/^docs\//, '').toLowerCase()}`
}

function documentSlug(sitePath: string) {
  return '/' + sitePath.replace(markdown, '').replace(/(?:^|\/)README$/i, '')
}

export const parseFrontMatter: ParseFrontMatter = async params => {
  const parsed = await params.defaultParseFrontMatter(params)
  const relative = path.relative(repositoryRoot, params.filePath)
  const sitePath = routeForDocument(relative)
  const companion = companionFromPath(relative)
  if (sitePath) {
    if (companion || typeof parsed.frontMatter.slug !== 'string') {
      parsed.frontMatter.slug = documentSlug(sitePath)
    } else if (!parsed.frontMatter.slug.startsWith('/')) {
      parsed.frontMatter.slug = path.posix.join('/', path.dirname(sitePath), parsed.frontMatter.slug)
    }
  }

  if (companion) parsed.frontMatter.custom_edit_url = null
  return parsed
}

function githubFile(repository: string, revision: string, file: string, type: 'blob' | 'raw' | 'tree') {
  const filePath = file.split('/').map(encodeURIComponent).join('/')
  return type === 'raw'
    ? `https://raw.githubusercontent.com/${repository}/${revision}/${filePath}`
    : `https://github.com/${repository}/${type}/${revision}/${filePath}`
}

function sourceMarkdown(repositoryPath: string) {
  if (!markdown.test(repositoryPath)) return false
  const companion = companionFromPath(repositoryPath)
  if (companion) return companion.paths.includes(repositoryPath.slice(companionPath(companion, '').length))
  if (/^docs\/(?:site|codex|feed)\//.test(repositoryPath)) return false
  return routeForDocument(repositoryPath) !== undefined && existsSync(path.resolve(repositoryRoot, repositoryPath))
}

function localMarkdownLink(originalPath: string, target: string, fragment: string) {
  let relative = path.relative(path.dirname(originalPath), target)
  if (!relative.startsWith('.')) relative = `./${relative}`
  return relative.split('/').map(part => encodeURIComponent(part)).join('/') + fragment
}

function importedGitHubLink(url: string, originalPath: string) {
  const match = /^https:\/\/github\.com\/([^/]+\/[^/]+)\/blob\/([^/]+)\/([^?#]+)(.*)$/i.exec(url)
  if (!match) return undefined
  const [, repository, revision, file, fragment] = match
  const companion = (remoteSources as readonly Source[]).find(
    source => source.repository.toLowerCase() === repository.toLowerCase(),
  )
  if (companion ? (revision !== companion.revision && revision !== 'main') : (
    repository.toLowerCase() !== defaultRepository.toLowerCase() || revision !== 'main'
  )) return undefined
  const original = decodeURIComponent(file)
  const target = companion ? companionPath(companion, original) : original
  // Keep the pinned source attribution on imported pages pointing upstream.
  if (target === originalPath || !sourceMarkdown(target)) return undefined
  return localMarkdownLink(originalPath, target, fragment)
}

function rewriteLink(node: MarkdownNode, originalPath: string) {
  const url = node.url
  if (!url || !['link', 'image', 'definition'].includes(node.type)) return
  if (url.startsWith('https://github.com/') && node.type !== 'image') {
    node.url = importedGitHubLink(url, originalPath) ?? url
    return
  }
  if (/^(?:[a-z\w+.-]+:|\/\/|#|\?)/i.test(url)) return

  const companion = companionFromPath(originalPath)
  if (!companion && url.startsWith('/')) return
  const [encodedTarget] = url.split(/[?#]/)
  const suffix = url.slice(encodedTarget.length)
  const sourceDirectory = companion ? companionPath(companion, '') : ''
  const originalSourcePath = originalPath.slice(sourceDirectory.length)
  const sourceTarget = path.posix.normalize(encodedTarget.startsWith('/')
    ? decodeURIComponent(encodedTarget.slice(1))
    : path.posix.join(path.dirname(originalSourcePath), decodeURIComponent(encodedTarget)))
  if (sourceTarget === '..' || sourceTarget.startsWith('../')) return
  const target = sourceDirectory + sourceTarget
  if (node.type !== 'image' && sourceMarkdown(target)) {
    node.url = localMarkdownLink(originalPath, target, suffix)
    return
  }

  const targetPath = path.resolve(repositoryRoot, target)
  if (node.type === 'image' && existsSync(targetPath)) {
    node.url = localMarkdownLink(originalPath, target, suffix)
    return
  }
  if (!companion && !existsSync(targetPath)) return

  const repository = companion?.repository ?? hostRepository
  const revision = companion?.revision ?? hostRevision
  const type = node.type === 'image' ? 'raw' : encodedTarget.endsWith('/') ? 'tree' : 'blob'
  node.url = githubFile(repository, revision, sourceTarget, type) + suffix
}

export function resolveDocSourceLink() {
  return (tree: MarkdownNode, file: {path: string}) => {
    const originalPath = path.relative(repositoryRoot, file.path)
    if (!routeForDocument(originalPath)) return
    const definitions = new Map<string, MarkdownNode>()
    const images: MarkdownNode[] = []
    function visit(node: MarkdownNode) {
      if (node.identifier && node.type === 'definition') definitions.set(node.identifier, node)
      if (node.type === 'imageReference') images.push(node)
      node.children?.forEach(visit)
    }
    visit(tree)

    for (const image of images) {
      const definition = image.identifier ? definitions.get(image.identifier) : undefined
      if (!definition?.url) continue
      image.type = 'image'
      image.url = definition.url
      image.title = definition.title
    }
    function rewrite(node: MarkdownNode) {
      rewriteLink(node, originalPath)
      node.children?.forEach(rewrite)
    }
    rewrite(tree)
  }
}
