FROM ubuntu:24.04

ARG RELAY_VERSION

LABEL org.opencontainers.image.source="https://github.com/matthewdavidrichardanderson/dusklight-online-mod"
LABEL org.opencontainers.image.description="Dusklight Online relay for Gluetun"

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        curl \
        jq \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home relay

COPY dusklight_online_relay /usr/local/bin/dusklight_online_relay
COPY entrypoint.sh /usr/local/bin/dusklight-relay-entrypoint

RUN chmod 0755 /usr/local/bin/dusklight_online_relay /usr/local/bin/dusklight-relay-entrypoint \
    && test -n "$RELAY_VERSION" \
    && test "$(/usr/local/bin/dusklight_online_relay --version)" = \
        "dusklight_online_relay ${RELAY_VERSION}"

USER relay
ENTRYPOINT ["/usr/local/bin/dusklight-relay-entrypoint"]
