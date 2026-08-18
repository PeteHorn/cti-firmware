#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="${DOCKER_IMAGE_NAME:-cti-firmware-builder}"
BUILD_DIR_HOST="${BUILD_DIR:-$ROOT_DIR/build-docker}"
ARTIFACT_DIR_HOST="${ARTIFACT_DIR:-$ROOT_DIR/docker-artifacts}"
DEPLOY_DIR_HOST="${DEPLOY_DIR:-}"
DESCRIPTION="${DESCRIPTION:-}"
BUILD_DIR_CONTAINER="/workspace/build-docker"
ARTIFACT_DIR_CONTAINER="/workspace/docker-artifacts"
DOCKERFILE_DIR="$ROOT_DIR/ci"
REBUILD_IMAGE=0
INSTALL_DOCKER_ENGINE=0

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  --rebuild-image            Force rebuild of the Docker image
  --install-docker-engine    Install Ubuntu docker.io if Docker is missing
  --platform PLATFORM         Force docker build/run platform (eg linux/amd64)
  --build-dir DIR            Host build directory (default: ./build-docker)
  --artifact-dir DIR         Host artifact output directory (default: ./docker-artifacts)
  --deploy-dir DIR           Host deploy destination for built artifacts and metadata
  --description TEXT         Free-form release description / build notes
  --version VERSION          Override derived version for the build
  -h, --help                 Show this help message

Example:
  ./ci/docker_build.sh --rebuild-image --deploy-dir /tmp/deploy/v1.2.3 --description "New network feature support"
  ./ci/docker_build.sh --install-docker-engine
EOF
}

install_docker_engine() {
  if ! command -v apt-get >/dev/null 2>&1; then
    echo "apt-get is required to install Docker on this machine." >&2
    exit 1
  fi

  echo "Installing Docker Engine (docker.io) from Ubuntu repositories..."
  sudo apt-get update
  sudo apt-get install -y docker.io
  sudo systemctl enable --now docker || true
  echo "Docker Engine install complete."
}

while [ $# -gt 0 ]; do
  case "$1" in
    --rebuild-image)
      REBUILD_IMAGE=1
      shift
      ;;
    --install-docker-engine)
      INSTALL_DOCKER_ENGINE=1
      shift
      ;;
    --build-dir)
      mkdir -p "$2"
      BUILD_DIR_HOST="$(cd "$2" && pwd)"
      shift 2
      ;;
    --platform)
      BUILD_PLATFORM="$2"
      shift 2
      ;;
    --artifact-dir)
      mkdir -p "$2"
      ARTIFACT_DIR_HOST="$(cd "$2" && pwd)"
      shift 2
      ;;
    --deploy-dir)
      mkdir -p "$2"
      DEPLOY_DIR_HOST="$(cd "$2" && pwd)"
      shift 2
      ;;
    --description)
      DESCRIPTION="$2"
      shift 2
      ;;
    --version)
      VERSION="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

DOCKER_CMD="docker"

if ! command -v "$DOCKER_CMD" >/dev/null 2>&1; then
  if [ "$INSTALL_DOCKER_ENGINE" -eq 1 ]; then
    install_docker_engine
  fi
fi

# Map common uname -m values to docker platform strings
arch_to_platform() {
  case "$1" in
    x86_64|amd64) echo "linux/amd64" ;;
    aarch64|arm64) echo "linux/arm64" ;;
    armv7l|armhf) echo "linux/arm/v7" ;;
    *) echo "" ;;
  esac
}

HOST_ARCH="$(uname -m)"
HOST_PLATFORM="$(arch_to_platform "$HOST_ARCH")"

# If not provided, auto-detect platform from host architecture
if [ -z "${BUILD_PLATFORM-}" ]; then
  if [ -n "$HOST_PLATFORM" ]; then
    BUILD_PLATFORM="$HOST_PLATFORM"
  else
    BUILD_PLATFORM="linux/amd64"
  fi
fi

if [ -n "${BUILD_PLATFORM-}" ] && [ "$BUILD_PLATFORM" != "$HOST_PLATFORM" ]; then
  echo "Requested Docker image platform: $BUILD_PLATFORM"
  echo "Host platform: $HOST_PLATFORM"
  echo "Cross-platform Docker images can only run if qemu/binfmt support is enabled in your Docker daemon."
fi

if ! command -v "$DOCKER_CMD" >/dev/null 2>&1; then
  cat <<EOF >&2
Docker is required but not installed or not on PATH.
On Ubuntu, install the standard Docker Engine package:

  sudo apt-get update
  sudo apt-get install -y docker.io
  sudo usermod -aG docker "$(id -un)"

Then log out and log back in, or run the script again with sudo if necessary.
EOF
  exit 1
fi

if ! "$DOCKER_CMD" ps >/dev/null 2>&1; then
  # Try falling back to sudo if available and permitted
  if command -v sudo >/dev/null 2>&1; then
    if sudo "$DOCKER_CMD" ps >/dev/null 2>&1; then
      echo "Docker daemon accessible via sudo; using 'sudo docker' for commands."
      DOCKER_CMD="sudo $DOCKER_CMD"
    else
      cat <<EOF >&2
Docker command exists but cannot access the Docker daemon as the current user.
Try one of the following:

  1) Add your user to the docker group and relogin:

     sudo usermod -aG docker "$(id -un)"

  2) Run this script with sudo to use the root docker context:

     sudo $0 "$@"

  3) If you have sudo configured to allow non-interactive use, the script will
     automatically switch to 'sudo docker' when possible.

Then restart your shell and rerun this script.
EOF
      exit 1
    fi
  else
    cat <<EOF >&2
Docker command exists but cannot access the Docker daemon and 'sudo' is not available.
Install Docker Engine or configure permissions (add your user to the 'docker' group).

  sudo apt-get install -y docker.io
  sudo usermod -aG docker "$(id -un)"

Then log out and log back in, or run the script with sudo.
EOF
    exit 1
  fi
fi

ensure_buildx() {
  if ! "$DOCKER_CMD" buildx version >/dev/null 2>&1; then
    cat <<EOF >&2
Docker buildx is required for cross-platform builds.
Install it via your Docker package or enable the Docker CLI buildx plugin.
EOF
    exit 1
  fi
}

ensure_qemu() {
  echo "Registering qemu/binfmt support for cross-platform container execution..."
  "$DOCKER_CMD" run --rm --privileged tonistiigi/binfmt --install all >/dev/null 2>&1 || true
}

check_qemu_support() {
  "$DOCKER_CMD" run --rm --privileged tonistiigi/binfmt --test all >/dev/null 2>&1
}

build_with_docker() {
  echo "Building Docker image: $IMAGE_NAME (platform=$BUILD_PLATFORM)"
  if [ "$BUILD_PLATFORM" != "$HOST_PLATFORM" ]; then
    ensure_buildx
    # create/use a buildx builder for cross-platform builds
    "$DOCKER_CMD" buildx create --name cti-builder --use --driver docker-container >/dev/null 2>&1 || true
    "$DOCKER_CMD" buildx inspect --bootstrap >/dev/null 2>&1 || true
    "$DOCKER_CMD" buildx build --platform "$BUILD_PLATFORM" --load -t "$IMAGE_NAME" "$DOCKERFILE_DIR"
  else
    "$DOCKER_CMD" build -t "$IMAGE_NAME" "$DOCKERFILE_DIR"
  fi
}

if [ "$REBUILD_IMAGE" -eq 1 ] || ! "$DOCKER_CMD" image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  build_with_docker
fi

mkdir -p "$BUILD_DIR_HOST" "$ARTIFACT_DIR_HOST"

if [ -z "${VERSION-}" ]; then
  if git -C "$ROOT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
    VERSION="$(git -C "$ROOT_DIR" describe --tags --always --dirty 2>/dev/null || git -C "$ROOT_DIR" rev-parse --short HEAD)"
  else
    VERSION="local-$(date +%Y%m%d%H%M%S)"
  fi
fi

echo "Building in Docker image '$IMAGE_NAME'"
echo "Version = $VERSION"
echo "Host build dir = $BUILD_DIR_HOST"
echo "Host artifact dir = $ARTIFACT_DIR_HOST"

echo "Using Docker command: $DOCKER_CMD"

# Ensure image exists in the active docker daemon; rebuild if necessary
if ! $DOCKER_CMD image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  echo "Image $IMAGE_NAME not found in current docker daemon; rebuilding with $DOCKER_CMD"
  $DOCKER_CMD build --platform "$BUILD_PLATFORM" -t "$IMAGE_NAME" "$DOCKERFILE_DIR"
fi

echo "Local images (from active docker):"
$DOCKER_CMD images --format '{{.Repository}}:{{.Tag}} {{.ID}}' || true

# Check image platform vs requested build platform
IMG_ARCH="$($DOCKER_CMD image inspect --format '{{.Architecture}}' "$IMAGE_NAME" 2>/dev/null || true)"
IMG_OS="$($DOCKER_CMD image inspect --format '{{.Os}}' "$IMAGE_NAME" 2>/dev/null || true)"
IMG_PLATFORM="$($DOCKER_CMD image inspect --format '{{.Os}}/{{.Architecture}}' "$IMAGE_NAME" 2>/dev/null || true)"

echo "Built image metadata: os=$IMG_OS arch=$IMG_ARCH platform=$IMG_PLATFORM"
if [ -n "$IMG_PLATFORM" ] && [ "$BUILD_PLATFORM" != "$HOST_PLATFORM" ]; then
  echo "Requested platform ($BUILD_PLATFORM) differs from host platform ($HOST_PLATFORM)."
  echo "This script can only execute cross-platform images if qemu/binfmt support is available in Docker."
  echo "If container startup fails with 'cannot execute binary file', install qemu-user-static or enable Docker's binfmt support."
fi

if [ -n "$IMG_PLATFORM" ] && [ "$IMG_PLATFORM" != "$BUILD_PLATFORM" ]; then
  echo "Built image platform ($IMG_PLATFORM) does not match requested build platform ($BUILD_PLATFORM). Rebuilding image..."
  build_with_docker
  IMG_ARCH="$($DOCKER_CMD image inspect --format '{{.Architecture}}' "$IMAGE_NAME" 2>/dev/null || true)"
  IMG_OS="$($DOCKER_CMD image inspect --format '{{.Os}}' "$IMAGE_NAME" 2>/dev/null || true)"
  IMG_PLATFORM="$($DOCKER_CMD image inspect --format '{{.Os}}/{{.Architecture}}' "$IMAGE_NAME" 2>/dev/null || true)"
  echo "Rebuilt image metadata: os=$IMG_OS arch=$IMG_ARCH platform=$IMG_PLATFORM"
fi

if [ "$BUILD_PLATFORM" != "$HOST_PLATFORM" ]; then
  ensure_qemu
  if ! check_qemu_support; then
    cat <<EOF >&2
Cross-platform execution of image platform $BUILD_PLATFORM is not supported by this Docker daemon.
Install or enable qemu/binfmt support in Docker and retry.
On Ubuntu, install the qemu-user-static package or enable Docker's binfmt support.
EOF
    exit 1
  fi
fi

"$DOCKER_CMD" run --rm \
  -u "$(id -u):$(id -g)" \
  -e PICO_SDK_FETCH_FROM_GIT=ON \
  -e PICO_SDK_FETCH_FROM_GIT_TAG="${PICO_SDK_FETCH_FROM_GIT_TAG:-master}" \
  -e VERSION="$VERSION" \
  -v "$ROOT_DIR":/workspace \
  -v "$BUILD_DIR_HOST":"$BUILD_DIR_CONTAINER" \
  -v "$ARTIFACT_DIR_HOST":"$ARTIFACT_DIR_CONTAINER" \
  -w /workspace \
  --entrypoint /bin/bash \
  "$IMAGE_NAME" \
  -lc "mkdir -p '$BUILD_DIR_CONTAINER' '$ARTIFACT_DIR_CONTAINER' && cmake -S . -B '$BUILD_DIR_CONTAINER' -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico -G Ninja && cmake --build '$BUILD_DIR_CONTAINER' -- -j\"\$(nproc)\""

# Collect firmware artifacts from the build output into the host artifact folder.
cp -v "$BUILD_DIR_HOST"/*.uf2 "$ARTIFACT_DIR_HOST/" 2>/dev/null || true
cp -v "$BUILD_DIR_HOST"/*.bin "$ARTIFACT_DIR_HOST/" 2>/dev/null || true
cp -v "$BUILD_DIR_HOST"/*.hex "$ARTIFACT_DIR_HOST/" 2>/dev/null || true
cp -v "$BUILD_DIR_HOST"/*.elf "$ARTIFACT_DIR_HOST/" 2>/dev/null || true

pushd "$ARTIFACT_DIR_HOST" >/dev/null
ZIP_NAME="cti-${VERSION}.zip"
rm -f "$ZIP_NAME"
if command -v zip >/dev/null 2>&1; then
  zip -r "$ZIP_NAME" ./* >/dev/null
else
  python3 - <<'PY'
import os
import zipfile

base = os.getcwd()
zip_name = f"cti-{os.environ.get('VERSION', 'build')}.zip"
with zipfile.ZipFile(zip_name, 'w', compression=zipfile.ZIP_DEFLATED) as zf:
    for name in sorted(os.listdir(base)):
        full = os.path.join(base, name)
        if os.path.isfile(full):
            zf.write(full, arcname=name)
PY
fi
popd >/dev/null

if [ -n "$DEPLOY_DIR_HOST" ]; then
  echo "Deploying artifacts to $DEPLOY_DIR_HOST"
  mkdir -p "$DEPLOY_DIR_HOST"
  cp -av "$ARTIFACT_DIR_HOST"/* "$DEPLOY_DIR_HOST" 2>/dev/null || true
  cat > "$DEPLOY_DIR_HOST/build-info.txt" <<EOF
Version: $VERSION
Built: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
Source: $ROOT_DIR
Description:
$DESCRIPTION
EOF
  echo "Deployment complete. Metadata written to $DEPLOY_DIR_HOST/build-info.txt"
fi

echo "Docker build complete. Artifact: $ARTIFACT_DIR_HOST/$ZIP_NAME"
[ -n "$DEPLOY_DIR_HOST" ] && echo "Deployed artifacts to: $DEPLOY_DIR_HOST"
exit 0
