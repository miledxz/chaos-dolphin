# Build stage: compile and run the unit/integration tests; the image fails to build if they fail.
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src src
COPY tests tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCHAOS_WERROR=ON \
 && cmake --build build -j"$(nproc)" \
 && ./build/chaos_tests

FROM ubuntu:24.04
COPY --from=build /src/build/chaosproxy /usr/local/bin/chaosproxy
USER 10001
EXPOSE 8000 9100
ENTRYPOINT ["chaosproxy"]
CMD ["--help"]
