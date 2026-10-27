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
COPY THIRD_PARTY_NOTICES.md ./
COPY installer/ installer/
COPY native/ native/
COPY profiles/ profiles/

ENTRYPOINT ["python", "-m", "installer"]
