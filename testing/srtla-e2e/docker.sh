#!/bin/sh
# Run the harness under Linux in a container (4+ links, no root on the host).
#
#   docker.sh build [name=ref ...]        build receivers/sender/srtla_send into build-linux/
#   docker.sh run   VARIANTS [SCENARIOS] [run_matrix.py options...]
#   docker.sh shell                       interactive shell in the container
#
# The fork is bind-mounted; build.sh clones the BELABOX repositories inside the container into
# build-linux/deps/, and results land in results/<out>/ on the host like a native run.
# Env: IMAGE, BELABOX_SRT_REF, BELABOX_SRTLA_REF, CMAKE_EXTRA, RECEIVER_DEBUG=1 (receiver at debug
# log level, for heavy-logging builds).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../.." && pwd)
IMAGE=${IMAGE:-srtla-e2e}
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" -f "$HERE/Dockerfile" "$HERE"
mounts="-v $REPO:/work/srt -e BELABOX_SRT_REF -e BELABOX_SRTLA_REF"
mounts="$mounts -e CMAKE_EXTRA=${CMAKE_EXTRA:-} -e RECEIVER_DEBUG=${RECEIVER_DEBUG:-0}"
cmd=${1:-run}; [ $# -gt 0 ] && shift
case "$cmd" in
  build) exec docker run --rm $mounts -e BUILD_DIR=build-linux "$IMAGE" ./build.sh "$@" ;;
  run)   exec docker run --rm $mounts -e BUILD_DIR=build-linux "$IMAGE" ./run_matrix.py "$@" ;;
  shell) exec docker run --rm -it $mounts -e BUILD_DIR=build-linux "$IMAGE" bash ;;
  *) echo "usage: docker.sh build|run|shell ..."; exit 2 ;;
esac
