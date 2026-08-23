import { parseVersionParts } from './hermes_version'

/**
 * Hermes announcement feed.
 *
 * Messages are published as a JSON document on the repository's `feed` branch,
 * deliberately outside `main`: editing an announcement must not run CI, must
 * not force-move the `nightly` tag, and must not make every installation report
 * that a new nightly is available.
 *
 * The feed is remote content and is treated as untrusted throughout — every
 * field is validated here, and the component renders the result as text rather
 * than markup.
 */

export const DEFAULT_FEED_URL = "https://raw.githubusercontent.com/MrOz59/Hermes/feed/feed.json";

export const DISMISSED_STORAGE_KEY = "hermes.feed.dismissed";

/** Alert levels, in the order they are shown. Doubles as the allow-list. */
const LEVEL_ORDER = ["danger", "warning", "info", "success"];
const DEFAULT_LEVEL = "info";

/** Release channels a message may target. */
const CHANNELS = ["all", "stable", "nightly"];

/** Upper bound on rendered messages, so a bad feed cannot bury the home page. */
const MAX_MESSAGES = 10;

/**
 * Classify a build as `stable` (exactly `X.Y.Z`) or `nightly` (anything with a
 * suffix: `0.5.1+abc1234` from CI, `0.5.1.abc1234` or `0.5.1.dirty` from a
 * local branch build).
 */
export function releaseChannel(version) {
  return /^v?\d+\.\d+\.\d+$/.test(`${version ?? ""}`.trim()) ? "stable" : "nightly";
}

function compareParts(a, b) {
  for (let i = 0; i < 3; i++) {
    if (a[i] !== b[i]) {
      return a[i] < b[i] ? -1 : 1;
    }
  }
  return 0;
}

/**
 * Evaluate one comparison clause (`>=0.5.0`, `<0.5.2`, `0.5.1`) against a
 * parsed version.
 */
function satisfiesClause(parts, clause) {
  const match = `${clause}`.match(/^(>=|<=|>|<|=)?v?(\d+)\.(\d+)\.(\d+)$/);
  if (!match) {
    return false;
  }
  const result = compareParts(parts, match.slice(2, 5).map(value => Number.parseInt(value, 10)));
  switch (match[1] ?? "=") {
    case ">=":
      return result >= 0;
    case "<=":
      return result <= 0;
    case ">":
      return result > 0;
    case "<":
      return result < 0;
    default:
      return result === 0;
  }
}

/**
 * Does `version` fall inside the message's `affects` range?
 *
 * A range is space-separated clauses that must all hold (`">=0.5.0 <0.5.2"`);
 * an array of ranges matches if any one of them does. An absent range, or
 * `"*"`, matches everything. Build metadata is ignored, so a known issue in
 * 0.5.1 also covers every 0.5.1 nightly.
 */
export function affectsVersion(affects, version) {
  if (affects === null || affects === undefined || affects === "*") {
    return true;
  }
  const ranges = (Array.isArray(affects) ? affects : [affects]).filter(range => typeof range === "string");
  if (ranges.length === 0) {
    return true;
  }
  const parts = parseVersionParts(version);
  if (!parts) {
    // The installed version is unreadable, so a targeted message cannot be
    // shown honestly. Only unrestricted messages get through.
    return false;
  }
  return ranges.some(range => {
    const clauses = range.trim().split(/\s+/).filter(Boolean);
    return clauses.length > 0 && clauses.every(clause => satisfiesClause(parts, clause));
  });
}

function parseTimestamp(value) {
  if (typeof value !== "string" || !value.trim()) {
    return null;
  }
  const timestamp = Date.parse(value.trim());
  return Number.isNaN(timestamp) ? null : timestamp;
}

/**
 * Accept a link only if it is an absolute http(s) URL, so a `javascript:` or
 * `data:` href in the feed can never reach an anchor.
 */
function parseLink(raw) {
  const url = typeof raw === "string" ? raw : raw?.url;
  if (typeof url !== "string") {
    return null;
  }
  let parsed;
  try {
    parsed = new URL(url.trim());
  } catch {
    return null;
  }
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:") {
    return null;
  }
  const label = typeof raw?.label === "string" ? raw.label.trim() : "";
  return { url: parsed.href, label: label || null };
}

/**
 * Validate one raw entry into the shape the component renders, or return null
 * if it is unusable. An entry needs at least an `id` and a `title`.
 */
export function normalizeMessage(raw) {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) {
    return null;
  }
  const id = typeof raw.id === "string" ? raw.id.trim() : "";
  const title = typeof raw.title === "string" ? raw.title.trim() : "";
  if (!id || !title) {
    return null;
  }
  const level = LEVEL_ORDER.includes(raw.level) ? raw.level : DEFAULT_LEVEL;
  const channel = CHANNELS.includes(raw.channel) ? raw.channel : "all";
  const body = typeof raw.body === "string" ? raw.body.trim() : "";
  const revision = Number.isFinite(raw.revision) ? Math.trunc(raw.revision) : 1;
  return {
    id,
    title,
    body: body || null,
    level,
    channel,
    link: parseLink(raw.link),
    affects: raw.affects ?? null,
    starts: parseTimestamp(raw.starts),
    expires: parseTimestamp(raw.expires),
    // Opt-out rather than opt-in: an announcement the user cannot clear is the
    // exception, reserved for messages that must stay put.
    dismissible: raw.dismissible !== false,
    revision,
  };
}

/** Turn a fetched feed document into validated messages, dropping bad entries. */
export function parseFeed(document) {
  const entries = Array.isArray(document) ? document :
    Array.isArray(document?.messages) ? document.messages : [];
  const seen = new Set();
  const messages = [];
  for (const entry of entries) {
    const message = normalizeMessage(entry);
    if (!message || seen.has(message.id)) {
      continue;
    }
    seen.add(message.id);
    messages.push(message);
  }
  return messages;
}

/**
 * A message stays dismissed until its `revision` is bumped, which is how an
 * edited announcement comes back for people who already cleared it.
 */
export function isDismissed(message, dismissed) {
  if (!message.dismissible) {
    return false;
  }
  const seenRevision = dismissed?.[message.id];
  return Number.isFinite(seenRevision) && seenRevision >= message.revision;
}

/** Filter to the messages this build should show, most severe first. */
export function selectMessages(messages, { version = null, now = Date.now(), dismissed = {} } = {}) {
  const channel = releaseChannel(version);
  return messages
    .filter(message => message.channel === "all" || message.channel === channel)
    .filter(message => affectsVersion(message.affects, version))
    .filter(message => message.starts === null || message.starts <= now)
    .filter(message => message.expires === null || message.expires > now)
    .filter(message => !isDismissed(message, dismissed))
    // Array.prototype.sort is stable, so messages of equal severity keep the
    // order the feed author gave them.
    .sort((a, b) => LEVEL_ORDER.indexOf(a.level) - LEVEL_ORDER.indexOf(b.level))
    .slice(0, MAX_MESSAGES);
}

function storage() {
  try {
    return globalThis.localStorage ?? null;
  } catch {
    // Blocked by the browser's site-data settings.
    return null;
  }
}

/** Read the `{ id: revision }` map of messages this browser has cleared. */
export function loadDismissed() {
  try {
    const stored = JSON.parse(storage()?.getItem(DISMISSED_STORAGE_KEY) ?? "null");
    return stored && typeof stored === "object" && !Array.isArray(stored) ? stored : {};
  } catch {
    return {};
  }
}

export function saveDismissed(dismissed) {
  try {
    storage()?.setItem(DISMISSED_STORAGE_KEY, JSON.stringify(dismissed));
  } catch {
    // Dismissal is a convenience; losing it is not worth failing the page over.
  }
}

export async function fetchFeed(url = DEFAULT_FEED_URL, { signal } = {}) {
  const response = await fetch(url, { cache: "no-cache", credentials: "omit", signal });
  if (!response.ok) {
    throw new Error(`Announcement feed request failed (${response.status})`);
  }
  return parseFeed(await response.json());
}
