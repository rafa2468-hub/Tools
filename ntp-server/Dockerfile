# Build stage: compile a fully static binary.
FROM alpine:3.20 AS build
RUN apk add --no-cache g++ make
WORKDIR /src
COPY Makefile .
COPY src ./src
RUN make static

# Final stage: just the static binary, no shell, no libc.
FROM scratch
COPY --from=build /src/ntp-server /ntp-server
EXPOSE 123/udp
ENTRYPOINT ["/ntp-server"]
CMD ["-a", "0.0.0.0", "-p", "123", "-s", "1", "-r", "LOCL"]
