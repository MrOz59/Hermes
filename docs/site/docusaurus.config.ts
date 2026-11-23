import type {Config} from '@docusaurus/types';
import type {Options, ThemeConfig} from '@docusaurus/preset-classic';
import {themes as prismThemes} from 'prism-react-renderer';
import {parseFrontMatter, resolveDocSourceLink} from './content-routing';
import {companionPath, localDocs, remoteSources} from './content-sources.mjs';

const repository = 'https://github.com/MrOz59/Hermes';
const publishedUrl = process.env.HERMES_DOCS_URL;
const companionDocs = remoteSources.flatMap(source => source.paths.map(path => companionPath(source, path)));

export default {
  title: 'Hermes documentation',
  tagline: 'Self-hosted game streaming on Linux',
  url: publishedUrl ?? 'http://localhost:3000',
  baseUrl: process.env.HERMES_DOCS_BASE_URL ?? '/',
  noIndex: !publishedUrl,

  favicon: 'hermes.ico',
  staticDirectories: ['.generated', '../../src_assets/common/branding'],
  onBrokenLinks: 'throw',
  onBrokenAnchors: 'throw',
  onDuplicateRoutes: 'throw',
  markdown: {
    format: 'detect',
    parseFrontMatter,
    hooks: {onBrokenMarkdownLinks: 'throw'},
  },

  presets: [
    ['classic', {
      docs: {
        path: '../..',
        routeBasePath: '/',
        sidebarPath: './sidebars.ts',
        include: [...localDocs, ...companionDocs],
        exclude: ['**/_*.{md,mdx}', '**/_*/**', '**/*.test.{md,mdx}', '**/__tests__/**', 'docs/codex/**', 'docs/feed/**', 'docs/site/**'],
        beforeDefaultRemarkPlugins: [resolveDocSourceLink],
        editUrl: ({docPath}) => docPath.startsWith('docs/') ? repository + '/edit/main/' + docPath : undefined,
      },
      blog: false,
      theme: {customCss: './src/css/custom.css'},
      sitemap: publishedUrl ? {} : false,
    } satisfies Options],
  ],

  themes: [
    ['@easyops-cn/docusaurus-search-local', {
      indexBlog: false,
      forceIgnoreNoIndex: !publishedUrl,
      docsRouteBasePath: '/',
    }],
  ],

  themeConfig: {
    colorMode: {respectPrefersColorScheme: true},
    docs: {
      sidebar: {autoCollapseCategories: true},
    },
    navbar: {
      title: 'Hermes',
      logo: {alt: '', src: 'hermes.svg', width: 28, height: 28},
      items: [
        {type: 'docSidebar', sidebarId: 'guides', label: 'Guides', position: 'left'},
        {type: 'docSidebar', sidebarId: 'developers', label: 'Developers', position: 'left'},
        {type: 'docSidebar', sidebarId: 'project', label: 'Project', position: 'left'},
        {href: repository + '/releases', label: 'Releases', position: 'right'},
        {href: repository, label: 'GitHub', position: 'right'},
      ],
    },
    footer: {
      links: [
        {
          title: 'Guides',
          items: [
            {label: 'Getting started', to: '/getting_started'},
            {label: 'Configuration', to: '/configuration'},
            {label: 'Troubleshooting', to: '/troubleshooting'},
          ],
        },
        {
          title: 'Developers',
          items: [
            {label: 'Build from source', to: '/building'},
            {label: 'HTTP API', to: '/api'},
            {label: 'C++ reference', href: 'pathname:///reference/cpp/index.htm'},
          ],
        },
        {
          title: 'Project',
          items: [
            {label: 'Roadmap', to: '/product-roadmap'},
            {label: 'Releases', href: repository + '/releases'},
            {label: 'GitHub', href: repository},
          ],
        },
      ],
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['bash', 'cpp', 'ini', 'json', 'toml', 'yaml'],
    },
  } satisfies ThemeConfig,
} satisfies Config;
