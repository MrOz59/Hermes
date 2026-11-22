export type VersionParts = [major: number, minor: number, patch: number];

export interface GithubRelease {
  name?: string | null;
  tag_name?: string;
  body?: string | null;
}

export function compareVersionParts(left: VersionParts, right: VersionParts): number {
  return left[0] - right[0] || left[1] - right[1] || left[2] - right[2];
}

/**
 * Parse the leading `MAJOR.MINOR.PATCH` of a version string, or return null.
 *
 * Anything after the patch number is ignored on purpose: a nightly stamps its
 * commit into the version (`0.5.1+abc1234`), and that build is still release
 * 0.5.1 for comparison purposes.
 */
export function parseVersionParts(version: unknown): VersionParts | null {
  if (!version) {
    return null;
  }
  const match = `${version}`.match(/^v?(\d+)\.(\d+)\.(\d+)/);
  return match ? [Number(match[1]), Number(match[2]), Number(match[3])] : null;
}

class HermesVersion {
  readonly release: GithubRelease | null;
  readonly version: string | null;
  readonly versionName: string | null | undefined;
  readonly versionTag: string | null | undefined;
  readonly versionParts: VersionParts | null;
  readonly versionMajor: number | null;
  readonly versionMinor: number | null;
  readonly versionPatch: number | null;

  constructor(release: GithubRelease | null = null, version: string | null = null) {
    if (release) {
      this.release = release;
      this.version = HermesVersion.releaseVersion(release);
      this.versionName = release.name;
      this.versionTag = release.tag_name;
    } else if (version) {
      this.release = null;
      this.version = version;
      this.versionName = null;
      this.versionTag = null;
    } else {
      throw new Error('Either release or version must be provided');
    }
    this.versionParts = this.parseVersion(this.version);
    this.versionMajor = this.versionParts ? this.versionParts[0] : null;
    this.versionMinor = this.versionParts ? this.versionParts[1] : null;
    this.versionPatch = this.versionParts ? this.versionParts[2] : null;
  }

  static releaseVersion(release: GithubRelease | null) {
    const tag = release?.tag_name ?? "";
    if (/^v?\d+\.\d+\.\d+/.test(tag)) {
      return tag;
    }

    // Hermes publishes a rolling prerelease under the fixed `nightly` tag.
    // Its semantic version is carried in the release name/body instead.
    const description = `${release?.name ?? ""}\n${release?.body ?? ""}`;
    return description.match(/(?:^|[^\d])v?(\d+\.\d+\.\d+)(?:[^\d]|$)/)?.[1] ?? null;
  }

  parseVersion(version: string | null) {
    return parseVersionParts(version);
  }

  isGreater(otherVersion: HermesVersion | string) {
    let otherVersionParts;
    if (otherVersion instanceof HermesVersion) {
      otherVersionParts = otherVersion.versionParts;
    } else if (typeof otherVersion === 'string') {
      otherVersionParts = this.parseVersion(otherVersion);
    } else {
      throw new Error('Invalid argument: otherVersion must be a HermesVersion object or a version string');
    }

    if (!this.versionParts || !otherVersionParts) {
      return false;
    }
    return compareVersionParts(this.versionParts, otherVersionParts) > 0;
  }

  isSame(otherVersion: HermesVersion | string) {
    if (this.isGreater(otherVersion)) {
      return false;
    }
    const other = otherVersion instanceof HermesVersion ? otherVersion : new HermesVersion(null, otherVersion);
    return !other.isGreater(this) && this.versionParts !== null && other.versionParts !== null;
  }
}

export default HermesVersion;
