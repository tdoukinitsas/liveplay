# Contributor documentation

For installation and show operation, start with the [LivePlay README](../README.md).

| Guide | Scope |
|---|---|
| [Development](DEVELOPMENT.md) | Prerequisites, build commands, checks and repository layout |
| [Server](../server/README.md) | Process configuration, services and concurrency |
| [Client](../client/README.md) | Electron/renderer responsibilities and UI state |
| [Bus architecture](BUS_ARCHITECTURE.md) | Audio graph, output binding, DSP and metering |
| [Ownership and persistence](OWNERSHIP_MODEL.md) | Project, server, user, machine and session state |
| [Build scripts](../scripts/README.md) | Build, version and localization helpers |
| [Audio end-to-end checks](../server/tests/e2e/README.md) | Test setup and suite selection |
| [Documentation site](../docs-site/README.md) | Local preview and API reference maintenance |
| [Packaging and signing](../SIGNING.md) | Current package signing configuration |
| [API audit](API_AUDIT.md) | Coverage, source evidence and implementation findings |

The [Developer API Reference](https://tdoukinitsas.github.io/liveplay/api/) is the authoritative HTTP/WebSocket contract documentation. Its [catalog](../docs-site/public/api/reference.json) contains endpoint/message schemas and examples; repository guides describe implementation details without copying that catalog.
