import type {Config} from '@docusaurus/types';
import type {Options, ThemeConfig} from '@docusaurus/preset-classic';
import {themes as prismThemes} from 'prism-react-renderer';

const repository = 'https://github.com/MrOz59/Hermes';
const publishedUrl = process.env.HERMES_DOCS_URL;

export default {
  title: 'Hermes documentation',
  tagline: 'Self-hosted game streaming on Linux',
  url: publishedUrl ?? 'http://localhost:3000',
  baseUrl: process.env.HERMES_DOCS_BASE_URL ?? '/',
  noIndex: !publishedUrl,

  staticDirectories: ['.generated'],
  onBrokenLinks: 'throw',
  onBrokenAnchors: 'throw',
  onDuplicateRoutes: 'throw',
  markdown: {
    format: 'detect',
    hooks: {onBrokenMarkdownLinks: 'throw'},
  },

  presets: [
    ['classic', {
      docs: {
        path: '..',
        routeBasePath: '/',
        sidebarPath: './sidebars.ts',
        exclude: ['**/_*.{md,mdx}', '**/_*/**', '**/*.test.{md,mdx}', '**/__tests__/**', 'codex/**', 'feed/**', 'site/**'],
        editUrl: repository + '/edit/main/docs/',
      },
      blog: false,
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
    navbar: {
      title: 'Hermes',
      items: [
        {type: 'docSidebar', sidebarId: 'hermes', label: 'Docs', position: 'left'},
        {href: 'pathname:///reference/cpp/index.htm', label: 'C++ reference', position: 'left'},
        {href: repository + '/releases', label: 'Releases', position: 'right'},
        {href: repository, label: 'GitHub', position: 'right'},
      ],
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['bash', 'cpp', 'ini', 'json', 'toml', 'yaml'],
    },
  } satisfies ThemeConfig,
} satisfies Config;
