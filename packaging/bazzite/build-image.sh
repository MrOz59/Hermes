#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

container_tool="${CONTAINER_TOOL:-podman}"
image_name="${IMAGE_NAME:-localhost/hermes-bazzite:latest}"
base_image="${BAZZITE_IMAGE:-ghcr.io/ublue-os/bazzite}"
base_tag="${BAZZITE_TAG:-stable}"
fedora_version="${FEDORA_VERSION:-}"
release="${HERMES_RELEASE:-latest}"
driver_ref="${HERMES_KMS_REF:-main}"
rpm_path="${HERMES_RPM:-packaging/bazzite/hermes.rpm}"
source_revision="${SOURCE_REVISION:-$(git rev-parse --verify HEAD 2>/dev/null || printf unknown)}"

context_dir=""
cleanup() {
  if [[ -n "${context_dir}" ]]; then
    rm -rf -- "${context_dir}"
  fi
}
trap cleanup EXIT

for command_name in "${container_tool}" curl jq; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    printf 'required command not found: %s\n' "${command_name}" >&2
    exit 1
  fi
done

if [[ -z "${fedora_version}" ]]; then
  if command -v skopeo >/dev/null 2>&1; then
    fedora_version="$(skopeo inspect \
      "docker://${base_image}:${base_tag}" | jq -r \
      '.Labels["org.opencontainers.image.version"] | split(".")[0]')"
  fi
  if [[ ! "${fedora_version}" =~ ^[0-9]+$ ]]; then
    printf 'could not detect the base Fedora release; set FEDORA_VERSION\n' >&2
    exit 1
  fi
fi

if [[ ! -f "${rpm_path}" ]]; then
  case "${release}" in
    latest) api_url=https://api.github.com/repos/MrOz59/Hermes/releases/latest ;;
    *) api_url="https://api.github.com/repos/MrOz59/Hermes/releases/tags/${release}" ;;
  esac
  asset_url="$(curl -fsSL "${api_url}" | jq -r \
    --arg suffix ".fc${fedora_version}.x86_64.rpm" \
    '.assets[] | select(.name | endswith($suffix)) | .browser_download_url' | head -n1)"
  if [[ -z "${asset_url}" || "${asset_url}" == null ]]; then
    printf 'no Fedora %s Hermes RPM found in release %s\n' \
      "${fedora_version}" "${release}" >&2
    exit 1
  fi
  curl -fL --retry 3 --output "${rpm_path}" "${asset_url}"
fi

# The repository can contain several gigabytes of build output and Git history.
# Create a minimal context containing only the files referenced by the
# Containerfile so local builds and CI do not upload all of it to BuildKit.
context_dir="$(mktemp -d "${TMPDIR:-/tmp}/hermes-bazzite-context.XXXXXX")"
install -D -m 0644 packaging/bazzite/Containerfile \
  "${context_dir}/packaging/bazzite/Containerfile"
cp -a packaging/bazzite/files "${context_dir}/packaging/bazzite/files"
install -D -m 0644 "${rpm_path}" \
  "${context_dir}/packaging/bazzite/hermes.rpm"
install -D -m 0644 src_assets/linux/misc/60-hermes.conf \
  "${context_dir}/src_assets/linux/misc/60-hermes.conf"
install -D -m 0644 src_assets/linux/misc/60-hermes.rules \
  "${context_dir}/src_assets/linux/misc/60-hermes.rules"
install -D -m 0755 src_assets/linux/misc/hermes-gamescope-launch \
  "${context_dir}/src_assets/linux/misc/hermes-gamescope-launch"
install -D -m 0755 src_assets/linux/misc/hermes-monitor-recovery \
  "${context_dir}/src_assets/linux/misc/hermes-monitor-recovery"
install -D -m 0644 apollo.svg "${context_dir}/apollo.svg"
install -D -m 0644 LICENSE "${context_dir}/LICENSE"

build_args=(
  --file packaging/bazzite/Containerfile
  --tag "${image_name}"
  --build-arg "BAZZITE_IMAGE=${base_image}"
  --build-arg "BAZZITE_TAG=${base_tag}"
  --build-arg "HERMES_RPM=packaging/bazzite/hermes.rpm"
  --build-arg "HERMES_KMS_REF=${driver_ref}"
  --build-arg "HERMES_REVISION=${source_revision}"
  --build-arg "WITH_ISOLATED_SESSIONS=${WITH_ISOLATED_SESSIONS:-0}"
)

if [[ -n "${MODULE_SIGN_KEY:-}" || -n "${MODULE_SIGN_CERT:-}" ]]; then
  if [[ ! -r "${MODULE_SIGN_KEY:-}" || ! -r "${MODULE_SIGN_CERT:-}" ]]; then
    printf 'MODULE_SIGN_KEY and MODULE_SIGN_CERT must both name readable files\n' >&2
    exit 1
  fi
  build_args+=(
    --no-cache
    --secret "id=module_sign_key,src=${MODULE_SIGN_KEY}"
    --secret "id=module_sign_cert,src=${MODULE_SIGN_CERT}"
  )
fi

"${container_tool}" build "${build_args[@]}" "${context_dir}"
printf '\nBuilt %s\n' "${image_name}"
printf 'For a rootful local build, switch with:\n'
printf '  sudo bootc switch --transport containers-storage %s\n' "${image_name}"
