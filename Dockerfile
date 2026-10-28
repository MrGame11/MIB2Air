# syntax=docker/dockerfile:1
FROM python:3.11-slim-trixie AS runtime

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    CC=clang-19

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-19 \
    && ln -s /usr/bin/clang-19 /usr/local/bin/clang \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
RUN pip install --no-cache-dir pyelftools==0.32 rich==15.0.0
# Only the toolchain lives in the image; setup.sh mounts the checkout read-only, so
# code updates need no rebuild and setup runs offline once this image exists.
ARG DEPS=unknown
LABEL org.wirelessbridge.deps=$DEPS

ENTRYPOINT ["python", "-m", "installer"]
