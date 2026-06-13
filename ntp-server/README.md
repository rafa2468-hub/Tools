# ntp-server

A minimal-footprint NTP server for embedded systems and other
resource-constrained devices. It implements the server side of NTPv4
(RFC 5905) client/server exchanges over UDP/123, using the host's system
clock as the time reference.

## Design goals

- **Small binary**: single source file, no external dependencies beyond
  the C standard library and POSIX sockets. Builds to well under 20 KB
  with `-Os` and stripped symbols.
- **No dynamic memory**: fixed-size buffers and structs only, no heap
  allocation in the request/response path.
- **Single-threaded, synchronous**: one `recvfrom`/`sendto` per request.
  Sufficient for the low request rates typical of small LANs and IoT
  fleets.
- **Minimal attack surface**: only responds to standard client (mode 3)
  requests. Anything else (NTP control/mode 6 queries, broadcast,
  symmetric modes, malformed/truncated packets) is silently dropped.
  Response size matches request size, so the server cannot be used as a
  bandwidth amplifier.

## Building

```sh
make            # dynamically linked binary, ./ntp-server
make static     # fully static binary (good for minimal root filesystems)
make clean
```

## Usage

```
ntp-server [options]
  -a <address>   Bind address (default 0.0.0.0)
  -p <port>      UDP port (default 123, requires root for <1024)
  -s <stratum>   NTP stratum to report, 1-15 (default 1)
  -r <refid>     Reference ID: 4-char code (stratum 1) or IPv4
                 address of upstream source (stratum > 1).
                 Default: LOCL
  -u <user>      Drop privileges to this user after binding
  -d             Daemonize (fork to background)
  -h             Show this help
```

### Examples

Run as a stratum-1 "local clock" reference on the standard port (requires
root to bind to 123), dropping privileges to `nobody` afterwards:

```sh
sudo ./ntp-server -u nobody
```

Run on an unprivileged port for testing:

```sh
./ntp-server -p 1123 -a 127.0.0.1
ntpdate -q -p 1 -u 127.0.0.1:1123
```

Advertise as stratum 2, citing an upstream server's address as the
reference ID:

```sh
./ntp-server -s 2 -r 192.0.2.1 -u nobody
```

## How it works

For each incoming client request the server:

1. Records the receive timestamp (T2) as early as possible.
2. Validates the packet is at least 48 bytes and has mode == 3 (client).
3. Builds a 48-byte reply: stratum/reference ID/precision come from the
   configuration, the client's transmit timestamp is echoed back as the
   origin timestamp, T2 is copied into the receive timestamp field, and
   the current time (T3) is placed in the transmit timestamp field just
   before sending.
4. Sends the reply back to the client's address.

The "reference timestamp" (last time the local clock was set) is recorded
once at startup.

## Deploying on TrueNAS SCALE

A `Dockerfile` and `docker-compose.yml` are included for running the
server as a container on TrueNAS SCALE.

### 1. Check that UDP/123 is free on the host

TrueNAS SCALE itself runs `chronyd` for system time sync. It normally
doesn't bind UDP/123, but verify before mapping the port:

```sh
ss -ulnp | grep :123
```

If something is already listening, either stop it (only if you understand
the impact on the host's own time sync) or map a different host port for
testing, e.g. `1230:123/udp`.

### 2. Build the image

Either build directly on the TrueNAS host (enable SSH under *System
Settings > Services* and log in), or build on another machine and push to
a registry (Docker Hub, GHCR, or a local registry) that the TrueNAS host
can pull from.

```sh
cd ntp-server
docker build -t ntp-server:latest .
```

The result is a `scratch`-based image containing only the ~1 MB static
binary.

### 3. Deploy as a Custom App

In the TrueNAS SCALE UI: **Apps > Discover Apps > Custom App** (the exact
label depends on your SCALE version).

- **Image repository**: `ntp-server`, **tag**: `latest`
- **Image pull policy**: `Never` (if you built the image locally on the
  host) or `IfNotPresent`/`Always` if pulling from a registry
- **Container entrypoint args** (optional, defaults shown):
  `-a 0.0.0.0 -p 123 -s 1 -r LOCL`
- **Port forwarding**: container port `123/udp` -> host port `123/udp`
  (or `1230/udp` if testing alongside an existing service)
- **Capabilities**: drop `ALL`, add `NET_BIND_SERVICE` (needed to bind
  port 123 as a non-root user)
- **User**: a non-root UID such as `1000:1000`
- **Storage**: none required; the container is stateless

The included `docker-compose.yml` mirrors this configuration if your
SCALE version supports compose-based custom apps, or if you'd rather run
it via `docker compose up -d` directly from a shell on the host.

### 4. Test it

From another machine on the network:

```sh
python3 - <<'EOF'
import socket, struct, time
NTP_DELTA = 2208988800
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2)
pkt = bytearray(48); pkt[0] = 0x23
s.sendto(pkt, ("<truenas-ip>", 123))
data, _ = s.recvfrom(48)
secs = struct.unpack_from("!Q", data, 40)[0] >> 32
print("server time:", time.ctime(secs - NTP_DELTA))
EOF
```

or point a real NTP/SNTP client (`chronyd`, `ntpdate`, `w32tm`, etc.) at
the TrueNAS host's IP.

## Limitations

- IPv4 only.
- No NTP authentication (MAC/autokey) or NTS.
- No leap-second table; the leap indicator is always set to "no warning".
  If the host clock is not itself synchronized to a trustworthy source,
  treat clients of this server accordingly.
- Single-threaded: under very high request rates, requests queue in the
  kernel socket buffer rather than being processed concurrently. This is
  intentional to keep the implementation simple and the memory footprint
  predictable; it has not been a practical limit for typical embedded
  device fleets.
