// Request diagnostics for the Web UI.
//
// A request that never reaches the host produces nothing in the Hermes log — it
// never arrived — and `fetch` rejects with a bare TypeError whose message is
// "Failed to fetch" and nothing else. That combination left the login and
// create-password failures in issue #14 invisible from both ends at once: no
// page message, no host log, no console output.
//
// apiFetch() closes that gap. It logs what was requested, where it actually
// resolved to, how long it took, and what came back — so a report can be a
// copy-paste of the console instead of a guess.

const PREFIX = '[hermes]';

/**
 * Everything about the page that changes how a request behaves. Logged on
 * failure because these are exactly the things that are wrong when a request
 * cannot reach a host that is demonstrably running.
 */
function pageContext() {
  return {
    pageUrl: window.location.href,
    origin: window.location.origin,
    protocol: window.location.protocol,
    host: window.location.host,
    // A page served over http:// cannot talk to the https:// API, and a page
    // opened from file:// has no origin to resolve a relative URL against.
    secureContext: window.isSecureContext,
    userAgent: navigator.userAgent,
  };
}

/**
 * Plain-language reading of a fetch rejection. The browser will not tell us
 * which of these it was — by design, since distinguishing them leaks
 * cross-origin information — so list them rather than guess.
 */
function rejectionHints(error) {
  if (error?.name === 'AbortError') {
    return ['The request was aborted, usually by navigating away or a timeout.'];
  }
  if (error?.name !== 'TypeError') {
    return [`Unexpected error type: ${error?.name}.`];
  }
  return [
    'The request never got a reply. The browser reports every one of these the same way, so it is one of:',
    '- the certificate was refused, or its exception was never accepted for this exact host and port',
    '- the connection was reset or refused (Hermes not running, or listening on a different address)',
    '- a client-certificate prompt was dismissed',
    '- an extension, proxy or firewall blocked it',
    'Opening the same URL directly in a tab usually shows which, because a navigation is allowed to explain itself where a fetch is not.',
  ];
}

/**
 * fetch() with diagnostics. Same signature and return value, so it is a drop-in
 * replacement; it only adds console output.
 *
 * @param {string} url Relative or absolute request URL.
 * @param {object} options Passed through to fetch().
 * @param {string} label Human name for the operation, e.g. "create password".
 * @returns {Promise<Response>} The fetch result, unchanged.
 */
export async function apiFetch(url, options = {}, label = 'request') {
  // Resolving the URL here is the point: every API call in the UI is written
  // relative, so a page reached at an unexpected path silently changes what
  // gets requested.
  const resolved = new URL(url, window.location.href).href;
  const method = options.method || 'GET';
  const startedAt = performance.now();

  console.info(`${PREFIX} ${label}: ${method} ${resolved}`);

  let response;
  try {
    response = await fetch(url, options);
  } catch (error) {
    const elapsed = Math.round(performance.now() - startedAt);
    console.error(
      `${PREFIX} ${label} FAILED after ${elapsed}ms — the request never reached Hermes.`
    );
    console.error(`${PREFIX} ${error.name}: ${error.message}`);
    // Timing separates the two shapes this takes: an immediate rejection is a
    // refused or reset connection, while seconds of waiting is a stall or a
    // dialog nobody answered.
    console.error(`${PREFIX} request context`, {
      label,
      method,
      requested: url,
      resolvedTo: resolved,
      elapsedMs: elapsed,
      ...pageContext(),
    });
    for (const hint of rejectionHints(error)) {
      console.error(`${PREFIX} ${hint}`);
    }
    throw error;
  }

  const elapsed = Math.round(performance.now() - startedAt);
  if (!response.ok) {
    console.error(
      `${PREFIX} ${label}: Hermes answered ${response.status} ${response.statusText} after ${elapsed}ms`
    );
    // The body carries the reason for the 4xx/5xx replies the UI sends. Read a
    // clone so the caller still gets an unread body.
    response
      .clone()
      .text()
      .then((body) => {
        if (body) {
          console.error(`${PREFIX} response body: ${body}`);
        }
      })
      .catch(() => {});
  } else {
    console.info(`${PREFIX} ${label}: ${response.status} in ${elapsed}ms`);
  }

  return response;
}

/**
 * One line at page load. It puts the page identity and the origin at the top of
 * the console, so a pasted report is self-describing even when the interesting
 * part is a single failure further down.
 */
export function logPageContext(page) {
  console.info(`${PREFIX} ${page} loaded`, pageContext());
}
