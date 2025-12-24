## Summary

Adds MCP (Model Context Protocol) support to llama-server's web UI:
- Tool calls via stdio transport
- Server manages MCP subprocesses, frontend connects via WebSocket
- Tool calls & results displayed in collapsible blocks

**Related work**: @allozaur is developing MCP integration based on #17487 (frontend-heavy approach). This PR takes a backend-heavy approach. Will coordinate after Christmas to find best path forward.

<img width="796" height="309" alt="Screenshot 2025-12-24 at 00 33 25" src="https://github.com/user-attachments/assets/879ab3b9-0ac3-4fe2-af9f-e32891fbb94d" />
<img width="414" height="284" alt="Screenshot 2025-12-24 at 00 33 37" src="https://github.com/user-attachments/assets/14f9bbe9-311b-4173-9676-4a4c256a083b" />
<img width="402" height="486" alt="Screenshot 2025-12-24 at 00 33 44" src="https://github.com/user-attachments/assets/1918e23d-33ed-42a1-8eee-b77564235a11" />
<img width="808" height="616" alt="Screenshot 2025-12-24 at 00 49 54" src="https://github.com/user-attachments/assets/5e5c9663-ff57-451b-b74d-60cffb9e7a80" />
<img width="790" height="465" alt="Screenshot 2025-12-24 at 00 51 38" src="https://github.com/user-attachments/assets/1cc8611b-c418-4dd7-af1e-333cfaef740d" />

### Usage

```bash
# Enable MCP support (config defaults to ~/.llama.cpp/mcp.json)
./llama-server -hf unsloth/Qwen3-Coder-30B-A3B-Instruct-1M-GGUF:UD-Q4_K_XL --webui-mcp
```

Use `--mcp-config /path/to/mcp.json` to override default config location.

### Configuration Example

```json
{
  "mcpServers": {
    "brave-search": {
      "command": "npx",
      "args": ["-y", "@anthropic-ai/mcp-server-brave-search"],
      "env": {
        "BRAVE_API_KEY": "..."
      }
    },
    "python": {
      "command": "uvx",
      "args": ["mcp-run-python"],
      "env": {}
    }
  }
}
```

### Security

WebSocket server includes:
- Payload size limits (10MB per frame, 100MB assembled)
- Connection limits (10 by default, configurable via `LLAMA_WS_MAX_CONNECTIONS`)
- Socket timeouts (30s) to prevent slow-loris attacks
- RFC 6455 compliance (RSV bits, client masking)

### Architecture

| File | Purpose |
|------|---------|
| `server-ws.cpp/h` | WebSocket server (on HTTP port + 1) |
| `server-mcp-bridge.cpp/h` | Routes WebSocket ↔ MCP subprocesses (uses `subprocess.h`) |
| `server-mcp.h` | MCP config types |
| `webui/src/lib/services/mcp.ts` | MCP SDK client wrapper |
| `webui/src/lib/stores/mcp.svelte.ts` | Connection state management |

### API

| Endpoint | Description |
|----------|-------------|
| `GET /mcp/servers` | List available MCP servers |
| `WS /mcp?server=<name>` | WebSocket connection (HTTP port + 1) |

## Test plan

- [x] Unit tests (`tools/server/tests/unit/test_mcp.py`)
- [x] Manual testing with filesystem and brave-search MCP servers
- [ ] WebSocket security tests (payload limits, connection limits)
- [ ] E2E tool calling in chat UI

## TODOs

- [x] Security hardening (payload limits, connection limits, timeouts)
- [x] Remove unused JSON-RPC types (~170 lines)
- [x] Use `subprocess.h` instead of custom process management (per @ngxson feedback)
- [ ] Coordinate with @allozaur / #17487 on best approach
- [ ] Consider SSE/streamable HTTP vs custom WS (per @ngxson feedback)
- [ ] Support more tool result types (images, resources)

## Possible follow-ups

- More MCP features: resources, prompts, logging
- SSE transport option
- [MCP Apps](https://github.com/anthropics/anthropic-quickstarts/tree/main/mcp-chat)

cc/ @allozaur @ngxson
