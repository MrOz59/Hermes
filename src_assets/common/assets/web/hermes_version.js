class HermesVersion {
  constructor(release = null, version = null) {
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

  static releaseVersion(release) {
    const tag = release?.tag_name ?? "";
    if (/^v?\d+\.\d+\.\d+/.test(tag)) {
      return tag;
    }

    // Hermes publishes a rolling prerelease under the fixed `nightly` tag.
    // Its semantic version is carried in the release name/body instead.
    const description = `${release?.name ?? ""}\n${release?.body ?? ""}`;
    return description.match(/(?:^|[^\d])v?(\d+\.\d+\.\d+)(?:[^\d]|$)/)?.[1] ?? null;
  }

  parseVersion(version) {
    if (!version) {
      return null;
    }
    const match = `${version}`.match(/^v?(\d+)\.(\d+)\.(\d+)/);
    return match ? match.slice(1, 4).map(value => Number.parseInt(value, 10)) : null;
  }

  isGreater(otherVersion) {
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
    for (let i = 0; i < 3; i++) {
      if (this.versionParts[i] > otherVersionParts[i]) {
        return true;
      } else if (this.versionParts[i] < otherVersionParts[i]) {
        return false;
      }
    }
    return false;
  }

  isSame(otherVersion) {
    if (this.isGreater(otherVersion)) {
      return false;
    }
    const other = otherVersion instanceof HermesVersion ? otherVersion : new HermesVersion(null, otherVersion);
    return !other.isGreater(this) && this.versionParts !== null && other.versionParts !== null;
  }
}

export default HermesVersion;
