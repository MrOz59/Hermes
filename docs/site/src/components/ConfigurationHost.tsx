import { useState } from 'react'

const tabs = [
  { id: 'general', label: 'General' },
  { id: 'input', label: 'Input' },
  { id: 'av', label: 'Audio/Video' },
  { id: 'network', label: 'Network' },
  { id: 'files', label: 'Config Files' },
  { id: 'advanced', label: 'Advanced' },
  { id: 'nv', label: 'NVIDIA NVENC Encoder' },
  { id: 'vaapi', label: 'VA-API Encoder' },
  { id: 'sw', label: 'Software Encoder' },
]

function configurationUrl(authority: string, tab: string): string | undefined {
  const host = authority.trim()
  if (!host || /[\s/\\@?#]/.test(host)) return

  try {
    const url = new URL(`https://${host}/config/`)
    url.hash = tab
    return url.href
  } catch {
    return undefined
  }
}

export default function ConfigurationHost() {
  const [authority, setAuthority] = useState('localhost:47990')
  const [tab, setTab] = useState(tabs[0].id)
  const url = configurationUrl(authority, tab)

  return (
    <fieldset>
      <legend>Open the Hermes Web UI</legend>
      <p>
        <label htmlFor="hermes-host">Host and port</label>{' '}
        <input
          id="hermes-host"
          type="text"
          value={authority}
          onChange={(event) => setAuthority(event.currentTarget.value)}
        />
      </p>
      <p>
        <label htmlFor="hermes-config-tab">Configuration tab</label>{' '}
        <select
          id="hermes-config-tab"
          value={tab}
          onChange={(event) => setTab(event.currentTarget.value)}
        >
          {tabs.map(({ id, label }) => <option key={id} value={id}>{label}</option>)}
        </select>
      </p>
      <p>
        {url
          ? <a href={url} target="_blank" rel="noreferrer">Open configuration (new tab)</a>
          : 'Enter a host name or IP address, with a port if needed.'}
      </p>
    </fieldset>
  )
}
