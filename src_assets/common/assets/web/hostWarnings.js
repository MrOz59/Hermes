/**
 * The banners the home page shows about the host's own readiness.
 *
 * This lives in a module rather than inline in index.html so it can be tested
 * without a browser. It used to be a computed feeding the root render with
 * nothing between it and the whole page: when `/api/config` returned a
 * `streamPorts` that was the string `"[]"` rather than an array - a read-only
 * field that had round-tripped through hermes.conf - the `|| []` guard did not
 * fire, because a non-empty string is truthy, `.filter` was not a function, and
 * Vue tore the tree down. `<body id="app" v-cloak>` then left an entirely blank
 * page, which reads as a server or auth failure rather than a render error.
 *
 * The server no longer produces that response. This still refuses to trust the
 * shape of what it is given, because the cost of being wrong is the whole page
 * rather than one missing banner.
 */

/** @returns {object} `value` when it is a plain object, an empty one otherwise. */
function asObject(value) {
  return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

/** @returns {Array} `value` when it is an array, an empty one otherwise. */
function asArray(value) {
  return Array.isArray(value) ? value : [];
}

/**
 * @param {object|null} config The `/api/config` response.
 * @returns {Array<{id: string, level: 'warning'|'danger', title: string, message: string, href: string, action: string}>}
 */
export function hostWarnings(config) {
  const c = asObject(config);
  const warnings = [];
  const evdiInfo = asObject(c.evdiInfo);
  const hermesKmsInfo = asObject(c.hermesKmsInfo);
  const clipboardInfo = asObject(c.clipboardInfo);
  const backend = c.virtual_display_backend;
  // Only the EVDI backend gets EVDI's readiness warnings. Treating
  // "not hermes_kms" as EVDI meant a host configured for no virtual-display
  // device at all was told to go install one, and every remedy it was
  // offered was a no-op there.
  const evdiBackendActive = c.platform === 'linux' && backend === 'evdi';
  const hermesKmsBackendActive = c.platform === 'linux' && backend === 'hermes_kms';

  if (evdiBackendActive && evdiInfo.diagnostic && evdiInfo.diagnostic !== 'ready') {
    warnings.push({
      id: 'evdi-' + evdiInfo.diagnostic,
      level: 'warning',
      title: 'Virtual display is not ready',
      message: `EVDI diagnostic: ${evdiInfo.diagnostic}. Hermes virtual-display sessions may fail until this is fixed. The Audio/Video tab has a step-by-step install guide.`,
      href: './config#Audio/Video',
      action: 'Open Audio/Video settings',
    });
  }

  if (hermesKmsBackendActive && hermesKmsInfo.diagnostic && hermesKmsInfo.diagnostic !== 'ready') {
    warnings.push({
      id: 'hermes-kms-' + hermesKmsInfo.diagnostic,
      level: 'warning',
      title: 'Hermes-KMS driver is not ready',
      message: `Hermes-KMS diagnostic: ${hermesKmsInfo.diagnostic}. Virtual-display sessions may fail until this is fixed. The Audio/Video tab has a step-by-step install guide for the Hermes-KMS driver.`,
      href: './config#Audio/Video',
      action: 'Open Audio/Video settings',
    });
  }

  if (c.platform === 'linux' && clipboardInfo.diagnostic && !clipboardInfo.available) {
    warnings.push({
      id: 'clipboard-' + clipboardInfo.diagnostic,
      level: 'warning',
      title: 'Clipboard sync is not ready',
      message: `Clipboard diagnostic: ${clipboardInfo.diagnostic}. Install wl-clipboard on Wayland or xclip on X11 if you want Hestia clipboard sync.`,
      href: './config#Audio/Video',
      action: 'Open clipboard setup',
    });
  }

  const busyPorts = asArray(c.streamPorts).filter(port => asObject(port).available === false);
  if (busyPorts.length > 0) {
    warnings.push({
      id: 'stream-port-in-use',
      level: 'danger',
      title: 'A port Hermes needs to stream is already taken',
      message: `${busyPorts.map(port => `${port.name} (${port.port})`).join(', ')} cannot be bound. `
        + 'Streams will fail the moment a client connects. Another Hermes may still be running, or '
        + 'another program has taken the port; changing the base port moves all of them at once.',
      href: './config#Network',
      action: 'Open network settings',
    });
  }

  if (evdiInfo.captureFallbackActive) {
    warnings.push({
      id: 'evdi-capture-fallback',
      level: 'danger',
      title: 'Virtual-display isolation fell back',
      message: 'The compositor did not activate the EVDI output, so Hermes is streaming a physical display to avoid black video.',
      href: './config#Audio/Video',
      action: 'Open EVDI diagnostics',
    });
  }

  if (asArray(evdiInfo.activeDisplays).some(display => asObject(display).zeroCopyCapture === false)) {
    warnings.push({
      id: 'evdi-cpu-buffer',
      level: 'warning',
      title: 'Virtual display uses a CPU capture copy',
      message: 'EVDI has no render node for direct VAAPI import. Hermes can still use hardware encoding, but the virtual-display capture path copies frames through system memory and may add latency.',
      href: './config#Audio/Video',
      action: 'Open EVDI diagnostics',
    });
  }

  if (c.platform === 'linux' && c.isolated_virtual_display_option && !evdiInfo.exclusiveLayoutSupported) {
    warnings.push({
      id: 'evdi-exclusive-layout',
      level: 'warning',
      title: 'Exclusive virtual-display layout is unavailable',
      message: 'Hermes cannot currently control this compositor output layout automatically. Physical monitors may remain enabled during virtual-display sessions.',
      href: './config#Audio/Video',
      action: 'Open Audio/Video settings',
    });
  }

  return warnings;
}
