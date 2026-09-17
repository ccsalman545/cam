# Protocol reference

Byte level formats of everything camstream puts on the wire or accepts.

## Ports and endpoints

| Endpoint | Transport | Purpose |
|---|---|---|
| `http://host:8080/` | TCP | dashboard |
| `http://host:8080/status` | TCP | JSON status |
| `http://host:8080/rtc/offer` | TCP | SDP signaling POST |
| `http://host:8080/rtc/close` | TCP | session teardown POST |
| `udp 50000 + slot` | UDP | one socket per WebRTC session: STUN, DTLS, RTP, RTCP |

## Datagram classification (RFC 7983)

| First byte | Class | Handling |
|---|---|---|
| 0 to 3 | STUN | binding checks answered, username validated |
| 20 to 63 | DTLS | routed into the session DTLS engine |
| >= 128 with second byte 192 to 223 | SRTCP (RTCP) | SRTP unprotect, feedback parsed |
| >= 128 otherwise | RTP | sendonly server, dropped |

(SRTCP packets carry V=2 in the first byte, i.e. 192 to 255 with the
padding bit clear; plain RTP is 128 to 191. Both must be classified as
the RTP class or browser NACK/PLI/FIR/BYE feedback is dropped.)

## STUN binding (RFC 5389)

Request (browser to server):

```
0                   1                   2                   3
| type = 0x0001     | message length   |
| magic cookie 0x2112A442              |
| transaction id (12 bytes)            |
| USERNAME (0x0006): "<server-ufrag>:<browser-ufrag>"
| PRIORITY (0x0024), USE-CANDIDATE (0x0025) when nominated
```

Response (server to browser), type 0x0101, same transaction id:

| Attribute | Content |
|---|---|
| XOR-MAPPED-ADDRESS (0x0020) | observed source IP and port |
| MESSAGE-INTEGRITY (0x0008) | HMAC-SHA1, key = server ice-pwd, message length includes the MI attribute |
| FINGERPRINT (0x0028) | CRC32 over the message XOR 0x5354554e |

## DTLS and SRTP

- DTLS 1.2, server role (`a=setup:passive`), ECDHE cipher suites, certificate generated per process run (ECDSA P-256).
- SRTP profile: `SRTP_AES128_CM_SHA1_80`.
- Key export: label `EXTRACTOR-dtls_srtp`, 60 bytes, order: client key (16), server key (16), client salt (14), server salt (14).
- The server sends with the server pair, receives with the client pair.

## RTP (RFC 3550)

```
0                   1                   2                   3
|1 0 V=2|P|X| CC=0  |M| payload type   | sequence number   |
| timestamp (90 kHz)                                    |
| SSRC                                                  |
| H.264 payload ...
```

- Payload type: taken from the browser offer (typically 96 to 127).
- Marker bit: set on the final packet of each access unit.
- Max packet size 1200 bytes.

## H.264 payload (RFC 6184)

- NAL units at or under 1188 bytes travel as single NAL packets.
- Larger NALs are FU-A: one indicator byte `(nri << 5) | 28`, one header byte with S and E bits plus the original type, then payload chunks.
- SPS and PPS precede every IDR (repeat headers enabled).

## RTCP

Sender Report (PT 200), 28 bytes:

| Field | Bytes |
|---|---|
| header (V=2, PT=200, length=6) | 4 |
| SSRC | 4 |
| NTP seconds + fraction (since 1900) | 8 |
| RTP timestamp equivalent | 4 |
| packet count | 4 |
| octet count | 4 |

Accepted inbound (all SRTP unprotected first):

| Packet | Detection | Action |
|---|---|---|
| Receiver Report (PT 201) | parsed for stats | logged at verbose level |
| BYE (PT 203) | source list | session closed |
| PLI (PT 206, FMT 1) | 12 bytes | IDR requested (400 ms rate limit) |
| FIR (PT 206, FMT 4) | 20 bytes | IDR requested |
| Generic NACK (PT 205, FMT 1) | FCI pairs (PID, bitmap) | cached packets resent verbatim |

## JSON API

`POST /rtc/offer` request body:

```json
{ "type": "offer", "sdp": "v=0\r\n..." }
```

Response:

```json
{ "type": "answer", "session_id": 174825277, "udp_port": 50000,
  "sdp": "v=0\r\n..." }
```

`POST /rtc/close` request: `{ "session_id": 174825277 }`, response `{ "closed": true }`.

`GET /status` returns the dashboard state: version, uptime, source, encoder, ports, captured and encoded counters, per session stats, and all local interface addresses.

## SDP answer shape (annotated)

```
v=0
o=- 1 1 IN IP4 <server-ip>
s=camstream
t=0 0
a=ice-lite                          # session-level only (RFC 8839)
a=ice-options:trickle
a=group:BUNDLE <video-mid>
a=msid-semantic: WMS camstream
a=fingerprint:sha-256 <cert>        # RFC 7999: hash algo prefix required
a=setup:passive                     # browser is DTLS client
a=ice-ufrag:<random8>               # STUN USERNAME starts with this
a=ice-pwd:<random24>
m=audio 0 UDP/TLS/RTP/SAVPF 0      # rejected (port 0) when the offer had audio
c=IN IP4 0.0.0.0
a=inactive
a=mid:<audio-mid>
m=video 9 UDP/TLS/RTP/SAVPF <pt>
c=IN IP4 <server-ip>
a=mid:<video-mid>
a=ice-ufrag:<random8>
a=ice-pwd:<random24>
a=fingerprint:sha-256 <cert>
a=setup:passive
a=sendonly
a=rtcp-mux
a=msid:camstream camstream-video
a=ssrc:<ssrc> cname:camstream
a=rtpmap:<pt> H264/90000
a=fmtp:<pt> packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1
a=rtcp-fb:<pt> nack
a=rtcp-fb:<pt> nack pli
a=rtcp-fb:<pt> ccm fir
a=candidate:1 1 udp 2130706431 <server-ip> <udp-port> typ host generation 0
a=end-of-candidates
```

The candidate line follows the RFC 8445 grammar
`candidate:<foundation> <component-id> <transport> <priority>
<connection-address> <port> typ <candidate-type>`; browsers reject
the whole answer if any of the integer fields is missing or out of
order.
