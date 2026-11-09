# Linux runtime for the SRTLA end-to-end harness. Linux routes the whole 127.0.0.0/8 on lo,
# so srtla_send can bind as many links as a scenario needs (127.0.0.2, .3, ...) without root
# or interface aliases. The fork is bind-mounted by docker.sh and build.sh clones the BELABOX
# repositories into its build directory; nothing is copied into the image.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git python3 ca-certificates procps \
    && rm -rf /var/lib/apt/lists/* \
    && git config --global --add safe.directory '*'
WORKDIR /work/srt/testing/srtla-e2e
