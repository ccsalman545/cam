<div align="center">

# camstream documentation

Everything beyond the [README](../README.md): architecture, protocols, deployment
guides, and reference tables.

[![Docs](https://img.shields.io/badge/pages-13-orange)](#index)
[![Diagrams](https://img.shields.io/badge/diagrams-Mermaid-blue)](#index)
[![Version](https://img.shields.io/badge/covers-2.0.0-informational)](#index)

</div>

---

## Pick a path

| You want to | Read |
|---|---|
| Understand how the server is built | [Architecture](10_architecture.md), then [WebRTC internals](11_webrtc_internals.md) |
| Get two machines talking over one cable | [LAN guide](13_lan_two_laptops.md) |
| Put it on a Raspberry Pi permanently | [Raspberry Pi guide](14_raspberry_pi.md), then the systemd unit inside it |
| Fix something that is broken | [Troubleshooting](17_troubleshooting.md) |
| Scale past 8 viewers | [Janus transport](22_janus_transport.md) |
| Audit the wire format | [Protocol reference](16_protocol_reference.md) |
| Build from source, static, or cross | [Build reference](12_build_reference.md) |

---

## Index

### Foundations

| Doc | Summary |
|---|---|
| [10. Architecture](10_architecture.md) | Layering rule, three-thread model, memory pools, the source and encoder abstractions, and the web UI contract. |
| [11. WebRTC internals](11_webrtc_internals.md) | Every protocol step the C server implements, from SDP parse to RTCP feedback, with the file that owns each. |
| [12. Build reference](12_build_reference.md) | Toolchain, library packages, every Make target and variable, static and cross builds, platform matrix. |

### Deployment

| Doc | Summary |
|---|---|
| [13. Two laptops, one cable](13_lan_two_laptops.md) | Static IPs, firewall rules, and verification with no router and no internet. |
| [14. Raspberry Pi guide](14_raspberry_pi.md) | Hardware encoder discovery, CSI camera bridge, per-model tuning, CPU load, and a systemd unit. |
| [20. Offline Ethernet deployment](20_webrtc_zero_latency.md) | The reference Pi-to-laptop path, its latency budget, and the correctness checks that prove it. |

### Transport backends

| Doc | Summary |
|---|---|
| [18. libpeer phase 3](18_libpeer_phase3.md) | Evaluation of sepfy/libpeer and the boundary where it meets the existing pipeline. |
| [21. libpeer runtime](21_libpeer_runtime.md) | Building `camstream-libpeer`, its signaling flow, known limitations, and backend switching. |
| [22. Janus transport](22_janus_transport.md) | `camstream-janus`: gateway configuration, port map, RTCP relay back to the encoder, embedded dashboard. |

### Reference

| Doc | Summary |
|---|---|
| [15. Optimization notes](15_optimization_notes.md) | Every change from the 1.x prototype to 2.0, with the measured or expected effect. |
| [16. Protocol reference](16_protocol_reference.md) | Byte-level formats for STUN, DTLS, RTP, RTCP, the JSON API, and the SDP answer. |
| [17. Troubleshooting](17_troubleshooting.md) | Decision tree plus symptom tables for build, camera, encoder, network, and hardware faults. |
| [19. Execution roadmap](19_execution_roadmap.md) | The original five-phase plan reconciled with the current checkout. |

The numbers are stable identifiers rather than an ordering. A new document takes
the next free number and nothing is renumbered, so links to these pages do not
break.

---

## Conventions used here

| Convention | Meaning |
|---|---|
| `code` | A file, flag, constant, or log fragment |
| ```sh``` blocks | Copy-paste runnable, from the repository root unless stated |
| Mermaid diagrams | Render on GitHub and in most Markdown viewers |
| RFC numbers | The normative source for the behaviour described |

Ports, flags, and field names in these documents match the 2.0.0 source. If a
document and the code disagree, the code wins: open an issue and the document
gets fixed.

---

## Keeping the docs honest

```sh
make check-docs        # same thing as ./tools/check_docs.sh
```

The check needs nothing but `python3`, and it fails on any of the following, so
documentation drift is caught before it ships:

- an em dash or en dash used as punctuation
- a relative link that points at a missing file
- an in-page anchor that does not match a heading
- a `docs/*.md` file that is missing from this index
- a `docs/*.md` file without a breadcrumb or a previous/next footer
- a fenced block that is unclosed, empty, or has no language tag

Diagram rendering is a separate concern: every `mermaid` block in this directory
and in the README parses with mermaid 11.

---

<div align="center">

[Back to the README](../README.md)

</div>
