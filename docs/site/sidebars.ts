import type { SidebarsConfig } from '@docusaurus/plugin-content-docs'

const sidebars: SidebarsConfig = {
  hermes: [
    'getting_started',
    'compatibility',
    {
      type: 'category',
      label: 'Configure and stream',
      items: [
        'configuration',
        'app_examples',
        'performance_tuning',
        'gamestream_migration',
        'container',
        'third_party_packages',
        'guides',
        { type: 'doc', id: 'awesome_sunshine', label: 'Community tools' },
      ],
    },
    {
      type: 'category',
      label: 'Troubleshooting',
      items: ['troubleshooting', 'hermes_kms_hdr', 'hdr-performance'],
    },
    {
      type: 'category',
      label: 'Developer',
      items: [
        'building',
        'contributing',
        'api',
        { type: 'link', label: 'C++ API', href: 'pathname:///reference/cpp/index.htm' },
        'versioning',
        'announcements',
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
    },
    {
      type: 'category',
      label: 'Project',
      items: [
        'changelog',
        'product-roadmap',
        'branding',
        'legal',
        {
          type: 'category',
          label: 'Experiments and release notes',
          items: [
            'experimental-isolated-gamescope',
            'libvirtualhid-experimental',
            'libvirtualhid-merge-handoff',
            'october-release-integration',
            'october-2026-rc3-notes',
            'october-rc2-notes',
          ],
        },
      ],
    },
  ],
}

export default sidebars
