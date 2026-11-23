import type { SidebarsConfig } from '@docusaurus/plugin-content-docs'

const sidebars: SidebarsConfig = {
  guides: [
    'getting_started',
    {
      type: 'category',
      label: 'Install Hermes',
      items: ['compatibility', 'third_party_packages', 'container'],
    },
    {
      type: 'category',
      label: 'Manage your host',
      items: ['configuration', 'app_examples', 'gamestream_migration'],
    },
    {
      type: 'category',
      label: 'Tune your stream',
      items: ['performance_tuning', 'hermes_kms_hdr', 'hdr-performance'],
    },
    {
      type: 'category',
      label: 'Find help',
      items: [
        'troubleshooting',
        'guides',
        { type: 'doc', id: 'awesome_sunshine', label: 'Community tools' },
      ],
    },
  ],
  developers: [
    'building',
    'contributing',
    {
      type: 'category',
      label: 'Reference',
      items: [
        { type: 'doc', id: 'api', label: 'HTTP API' },
        { type: 'link', label: 'C++ reference', href: 'pathname:///reference/cpp/index.htm' },
      ],
    },
    {
      type: 'category',
      label: 'Modularization',
      items: [
        'modularization/mcpp-migration',
        'modularization/baseline',
        'modularization/architecture-review',
        'modularization/implementation-sequence',
        'modularization/validation-plan',
        'modularization/build-system-review',
        'modularization/mcpp-adoption',
      ],
    },
  ],
  project: [
    'product-roadmap',
    {
      type: 'category',
      label: 'Releases and updates',
      items: [
        'changelog',
        'versioning',
        'announcements',
        'october-release-integration',
        'october-2026-rc3-notes',
        'october-rc2-notes',
      ],
    },
    {
      type: 'category',
      label: 'Experimental work',
      items: [
        'experimental-isolated-gamescope',
        'libvirtualhid-experimental',
        'libvirtualhid-merge-handoff',
      ],
    },
    'branding',
    'legal',
  ],
}

export default sidebars
