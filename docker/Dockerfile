# The environment Cascade engines are built and scored in (see benchmark.json).
#
#   docker build -t cascade-runtime:local docker
#
# Engines run as a non-root user with no network, a read-only root filesystem and
# their submission directory mounted read-only at /engine. HOME and CARGO_HOME are
# set to /tmp, which is writable.
FROM node:22-bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      python3 python-is-python3 build-essential ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain 1.85.0 --no-modify-path \
 && chmod -R a+rX "$RUSTUP_HOME" "$CARGO_HOME"

CMD ["sh"]
