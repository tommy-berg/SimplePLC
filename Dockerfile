# One SimplePLC = one PLC: a static binary on an empty image, for amd64, arm64 and 32-bit ARM
# (Raspberry Pi OS 32-bit: linux/arm/v7). The amd64 image is 11.8 MB (3.3 MB compressed), the
# arm/v7 image 8.9 MB.
#   docker build -t simpleplc .
#   docker run -p 8080:8080 -p 502:502 -p 4840:4840 -v plc1:/data simpleplc
# Several PLCs on one network, with a fleet for the panel and agents: README.md, "A network of PLCs".
# First-run settings (a new /data starts with them): SIMPLEPLC_ID (default: the container's host
# name), SIMPLEPLC_EXAMPLE (default none: a blank PLC; or an example such as tank), SIMPLEPLC_TOKEN
# (an administrator API token for scripts and agents, never stored). The setup code for the first
# panel account is printed to the container's log.
FROM alpine:3.20 AS toolchain
RUN apk add --no-cache build-base cmake git python3 linux-headers

# open62541 changes rarely: its own stage, so a change to SimplePLC does not rebuild it.
# The options match the CMake build with -DSIMPLEPLC_FETCH_OPEN62541=ON.
FROM toolchain AS open62541
ARG OPEN62541_TAG=v1.5.9
RUN git clone --depth 1 --branch "$OPEN62541_TAG" https://github.com/open62541/open62541.git /src \
 && cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -DUA_NAMESPACE_ZERO=REDUCED -DUA_ENABLE_ENCRYPTION=OFF -DUA_ENABLE_DISCOVERY_MULTICAST=OFF \
      -DUA_BUILD_EXAMPLES=OFF -DUA_BUILD_UNIT_TESTS=OFF -DUA_BUILD_TOOLS=OFF -DUA_FORCE_WERROR=OFF -DUA_LOGLEVEL=300 \
 && cmake --build /build -j"$(nproc)" \
 && cmake --install /build --prefix /opt/open62541

# The static binary (musl). Examples, the panel and the docs are embedded at build time, so the
# image needs nothing but the executable; .dockerignore keeps the rest of the repository out.
FROM toolchain AS build
COPY --from=open62541 /opt/open62541 /opt/open62541
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSIMPLEPLC_STATIC=ON -DCMAKE_PREFIX_PATH=/opt/open62541 \
 && cmake --build build -j"$(nproc)" --target simpleplc \
 && strip build/simpleplc \
 && mkdir -p /out/data

FROM scratch
COPY --from=build /src/build/simpleplc /simpleplc
# The data directory belongs to the unprivileged user the server runs as; Docker
# lets any user bind ports below 1024 inside the container, so 502 still works.
COPY --from=build --chown=65534:65534 /out/data /data
USER 65534:65534
ENV SIMPLEPLC_DATA=/data
VOLUME /data
EXPOSE 8080 502 4840
ENTRYPOINT ["/simpleplc"]
