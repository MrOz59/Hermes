export const localDocs = [
  'docs/**/*.{md,mdx}',
  'rust/**/README.md',
  'rust/**/docs/**/*.{md,mdx}',
  'mcpp/**/README.md',
  'mcpp/**/docs/**/*.{md,mdx}',
]

export const companionRoot = 'target/docs-content/companions'

export const remoteSources = [
  {
    id: 'hestia-rs',
    title: 'Hestia-rs',
    repository: 'MrOz59/hestia-rs',
    revision: '684986785952c513c950151fed4cc0f092220e60',
    license: 'GPL-3.0',
    paths: [
      'README.md',
      'docs/ARCHITECTURE.md',
      'docs/FRONTENDS.md',
      'docs/ROADMAP.md',
      'docs/VIDEO.md',
    ],
  },
  {
    id: 'hermes-kms',
    title: 'Hermes-KMS',
    repository: 'MrOz59/Hermes-KMS',
    revision: 'ba9aeb854d2b7368188a905598aa0217a855207a',
    license: 'GPL-2.0',
    paths: [
      'README.md',
      'CHANGELOG.md',
      'docs/configfs.md',
      'docs/driver-design.md',
      'docs/hdr-capture.md',
      'docs/installer-contract.md',
      'docs/references.md',
      'docs/releases/v0.4.1.md',
      'docs/roadmap.md',
    ],
  },
]

export function companionPath(source, path) {
  return `${companionRoot}/${source.id}/${path}`
}
