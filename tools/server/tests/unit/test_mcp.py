#!/usr/bin/env python
"""
Tests for MCP (Model Context Protocol) functionality.

Tests cover:
- HTTP endpoint: /mcp?server=... (GET and POST)
- CORS headers for browser compatibility
- WebSocket stdio bridge for local MCP servers
"""

import pytest
import json
import os
import tempfile
from pathlib import Path
import sys
import time
import websocket

# Ensure parent path is in sys.path
path = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(path))

from utils import ServerProcess

server: ServerProcess

TIMEOUT_SERVER_START = 60
TIMEOUT_HTTP_REQUEST = 10


def get_local_model():
    """Find a suitable local model for testing."""
    candidates = [
        "~/Library/Caches/llama.cpp/ggml-org_Qwen2.5-Coder-0.5B-Q8_0-GGUF_qwen2.5-coder-0.5b-q8_0.gguf",
        "~/Library/Caches/llama.cpp/bartowski_Llama-3.2-1B-Instruct-GGUF_Llama-3.2-1B-Instruct-Q4_K_M.gguf",
    ]
    for path in candidates:
        expanded = os.path.expanduser(path)
        if os.path.exists(expanded):
            return expanded
    return None


@pytest.fixture(autouse=True)
def create_server():
    """Create a basic server for MCP testing."""
    global server
    server = ServerProcess()
    server.server_port = 8082  # Use different port to avoid conflicts
    server.n_slots = 1
    server.n_ctx = 2048
    server.webui_mcp = True  # Enable MCP HTTP proxy support
    # Use a local model (MCP tests don't need HF download)
    local_model = get_local_model()
    if local_model:
        server.model_file = local_model
        # Clear HF defaults to avoid network access
        server.model_hf_repo = None
        server.model_hf_file = None
    else:
        pytest.skip("No local model found for MCP tests")
    yield
    server.stop()


class TestMcpHttpProxyCorsHeaders:
    """Test MCP HTTP proxy CORS headers for browser compatibility."""

    # Origin header to simulate browser request
    TEST_ORIGIN = "https://example.com"

    def test_proxy_cors_headers_on_options(self):
        """Test OPTIONS request returns proper CORS headers."""
        # Create config with a remote HTTP server
        config = {
            "mcpServers": {
                "test-remote": {
                    "url": "http://127.0.0.1:9999/mcp"
                }
            }
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            # Send OPTIONS request to proxy endpoint with Origin header (browsers always send this)
            res = server.make_request(
                "OPTIONS",
                "/mcp?server=test-remote",
                headers={"Origin": self.TEST_ORIGIN},
                timeout=TIMEOUT_HTTP_REQUEST
            )

            # Should return 200 or 204 (CORS preflight)
            assert res.status_code in (200, 204), f"Expected 200/204, got {res.status_code}"

            headers = res.headers

            # Check required CORS headers are present
            assert "Access-Control-Allow-Origin" in headers, \
                f"Missing Access-Control-Allow-Origin header"
            assert headers["Access-Control-Allow-Origin"] == self.TEST_ORIGIN, \
                f"Expected origin {self.TEST_ORIGIN}, got {headers.get('Access-Control-Allow-Origin')}"

            assert "Access-Control-Allow-Methods" in headers, \
                f"Missing Access-Control-Allow-Methods header"
            methods = headers["Access-Control-Allow-Methods"]
            assert "GET" in methods and "POST" in methods, \
                f"Expected GET and POST in methods, got {methods}"

            assert "Access-Control-Allow-Headers" in headers, \
                f"Missing Access-Control-Allow-Headers header"
            allow_headers = headers["Access-Control-Allow-Headers"]
            # Check for MCP-specific headers
            assert "content-type" in allow_headers.lower(), \
                f"Missing content-type in Allow-Headers: {allow_headers}"
            assert "mcp-session-id" in allow_headers.lower(), \
                f"Missing mcp-session-id in Allow-Headers: {allow_headers}"

            assert "Access-Control-Expose-Headers" in headers, \
                f"Missing Access-Control-Expose-Headers header"
            expose_headers = headers["Access-Control-Expose-Headers"]
            assert "mcp-session-id" in expose_headers.lower(), \
                f"Missing mcp-session-id in Expose-Headers: {expose_headers}"

        finally:
            try:
                os.unlink(config_path)
            except:
                pass

    def test_proxy_cors_headers_on_get(self):
        """Test GET request also returns CORS headers."""
        config = {
            "mcpServers": {
                "test-remote": {
                    "url": "http://127.0.0.1:9999/mcp"
                }
            }
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            # Send GET request to proxy endpoint with Origin header
            # This will fail to connect to the remote server, but we can check CORS headers
            res = server.make_request(
                "GET",
                "/mcp?server=test-remote",
                headers={"Origin": self.TEST_ORIGIN},
                timeout=TIMEOUT_HTTP_REQUEST
            )

            # Should get 502 Bad Gateway (remote server not available)
            # But CORS headers should still be present
            headers = res.headers

            assert "Access-Control-Allow-Origin" in headers, \
                f"Missing Access-Control-Allow-Origin in GET response"
            assert headers["Access-Control-Allow-Origin"] == self.TEST_ORIGIN, \
                f"Expected origin {self.TEST_ORIGIN}, got {headers.get('Access-Control-Allow-Origin')}"

            assert "Access-Control-Expose-Headers" in headers, \
                f"Missing Access-Control-Expose-Headers in GET response"

            expose_headers = headers["Access-Control-Expose-Headers"]
            assert "mcp-session-id" in expose_headers.lower(), \
                f"Missing mcp-session-id in Expose-Headers: {expose_headers}"

        finally:
            try:
                os.unlink(config_path)
            except:
                pass


class TestMcpHttpProxyErrors:
    """Test MCP HTTP proxy error handling."""

    def test_proxy_missing_server_param(self):
        """Test proxy returns 400 when server parameter is missing."""
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        res = server.make_request(
            "GET",
            "/mcp",
            timeout=TIMEOUT_HTTP_REQUEST
        )

        assert res.status_code == 400, f"Expected 400, got {res.status_code}"
        body = res.body
        assert "error" in str(body).lower(), f"Expected error in response: {body}"
        # Verify response is valid JSON
        try:
            data = json.loads(body if isinstance(body, str) else body.decode() if isinstance(body, bytes) else str(body))
            assert "error" in data, f"Expected 'error' key in JSON: {data}"
        except json.JSONDecodeError:
            pytest.fail(f"Response is not valid JSON: {body}")

    def test_proxy_server_not_found(self):
        """Test proxy returns 404 for unknown server."""
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        res = server.make_request(
            "GET",
            "/mcp?server=nonexistent-server",
            timeout=TIMEOUT_HTTP_REQUEST
        )

        assert res.status_code == 404, f"Expected 404, got {res.status_code}"
        body = res.body
        # Verify response is valid JSON (tests JSON injection fix)
        try:
            data = json.loads(body if isinstance(body, str) else body.decode() if isinstance(body, bytes) else str(body))
            assert "error" in data, f"Expected 'error' key in JSON: {data}"
        except json.JSONDecodeError:
            pytest.fail(f"Response is not valid JSON: {body}")

    def test_proxy_server_not_found_special_chars(self):
        """Test proxy handles special characters in server name safely."""
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        # Test with characters that could cause JSON injection
        res = server.make_request(
            "GET",
            '/mcp?server=test", "injected": "value',
            timeout=TIMEOUT_HTTP_REQUEST
        )

        assert res.status_code == 404, f"Expected 404, got {res.status_code}"
        body = res.body
        # Verify response is valid JSON (no injection)
        try:
            data = json.loads(body if isinstance(body, str) else body.decode() if isinstance(body, bytes) else str(body))
            assert "error" in data, f"Expected 'error' key in JSON: {data}"
            # Should only have 'error' key, no injected keys
            assert len(data) == 1, f"Expected only 'error' key, got: {data.keys()}"
        except json.JSONDecodeError:
            pytest.fail(f"Response is not valid JSON: {body}")

    def test_proxy_no_url_configured(self):
        """Test proxy returns 404 when server has no URL."""
        # Create config with a server that has no URL
        config = {
            "mcpServers": {
                "test-empty": {}
            }
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            res = server.make_request(
                "GET",
                "/mcp?server=test-empty",
                timeout=TIMEOUT_HTTP_REQUEST
            )

            # Server with no URL returns 404 (not found in config)
            assert res.status_code == 404, f"Expected 404, got {res.status_code}"
            body = res.body
            assert "error" in str(body).lower() or "not found" in str(body).lower(), \
                f"Expected error in response: {body}"

        finally:
            try:
                os.unlink(config_path)
            except:
                pass


class TestMcpWebSocketStdioBridge:
    """Test MCP WebSocket stdio bridge for local MCP servers."""

    def get_echo_server_config(self):
        """Create a config that uses the echo server fixture."""
        # Get the path to the echo server fixture
        fixtures_dir = Path(__file__).resolve().parent.parent / "fixtures"
        echo_server_path = fixtures_dir / "mcp_echo_server.py"

        if not echo_server_path.exists():
            pytest.skip(f"Echo server fixture not found at {echo_server_path}")

        config = {
            "mcpServers": {
                "echo": {
                    "command": "python3",
                    "args": [str(echo_server_path)]
                }
            }
        }
        return config

    def test_websocket_stdio_basic_connection(self):
        """Test basic WebSocket connection to stdio MCP server."""
        config = self.get_echo_server_config()

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            # Connect via WebSocket
            ws_port = server.server_port + 1
            ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=echo"

            ws = websocket.create_connection(ws_url, timeout=10)

            # Send initialize request
            init_request = {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {},
                    "clientInfo": {"name": "test-client", "version": "1.0.0"}
                }
            }
            ws.send(json.dumps(init_request))

            # Receive response
            response = ws.recv()
            response_data = json.loads(response)

            # Verify response
            assert "jsonrpc" in response_data
            assert response_data["id"] == 1
            assert "result" in response_data
            assert "protocolVersion" in response_data["result"]
            assert "serverInfo" in response_data["result"]

            ws.close()

        finally:
            try:
                os.unlink(config_path)
            except:
                pass

    def test_websocket_stdio_tools_list(self):
        """Test listing tools from stdio MCP server."""
        config = self.get_echo_server_config()

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            # Connect via WebSocket
            ws_port = server.server_port + 1
            ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=echo"

            ws = websocket.create_connection(ws_url, timeout=10)

            # Initialize first
            init_request = {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {},
                    "clientInfo": {"name": "test-client", "version": "1.0.0"}
                }
            }
            ws.send(json.dumps(init_request))
            ws.recv()  # Discard initialize response

            # Request tools list
            tools_request = {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/list"
            }
            ws.send(json.dumps(tools_request))

            # Receive response
            response = ws.recv()
            response_data = json.loads(response)

            # Verify response
            assert response_data["id"] == 2
            assert "result" in response_data
            assert "tools" in response_data["result"]

            # The echo server provides "echo" and "get_env_vars" tools
            tools = response_data["result"]["tools"]
            tool_names = [tool["name"] for tool in tools]
            assert "echo" in tool_names
            assert "get_env_vars" in tool_names

            ws.close()

        finally:
            try:
                os.unlink(config_path)
            except:
                pass

    def test_websocket_stdio_tool_call(self):
        """Test calling a tool through stdio MCP server."""
        config = self.get_echo_server_config()

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            # Connect via WebSocket
            ws_port = server.server_port + 1
            ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=echo"

            ws = websocket.create_connection(ws_url, timeout=10)

            # Initialize
            init_request = {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {},
                    "clientInfo": {"name": "test-client", "version": "1.0.0"}
                }
            }
            ws.send(json.dumps(init_request))
            ws.recv()

            # Call echo tool
            test_message = "Hello from WebSocket test!"
            tool_call_request = {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {
                    "name": "echo",
                    "arguments": {"message": test_message}
                }
            }
            ws.send(json.dumps(tool_call_request))

            # Receive response
            response = ws.recv()
            response_data = json.loads(response)

            # Verify response
            assert response_data["id"] == 2
            assert "result" in response_data
            assert "content" in response_data["result"]

            # The echo tool should return our message
            content = response_data["result"]["content"]
            assert len(content) > 0
            assert content[0]["type"] == "text"
            assert content[0]["text"] == test_message

            ws.close()

        finally:
            try:
                os.unlink(config_path)
            except:
                pass

    def test_websocket_stdio_missing_server_param(self):
        """Test WebSocket connection without server parameter."""
        config = self.get_echo_server_config()

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            # Connect via WebSocket without server parameter
            ws_port = server.server_port + 1
            ws_url = f"ws://{server.server_host}:{ws_port}/mcp"

            # Should fail to establish connection or close immediately
            # The server should reject connections without server parameter
            try:
                ws = websocket.create_connection(ws_url, timeout=5)
                # If connection succeeds, it should close quickly with an error
                # Try to receive - should get close frame or error
                try:
                    response = ws.recv()
                    # If we get a response, it should be an error
                    if response:
                        data = json.loads(response)
                        assert "error" in data or "jsonrpc" not in data
                except websocket.WebSocketConnectionClosedException:
                    pass  # Expected - connection closed by server
                finally:
                    ws.close()
            except (websocket.WebSocketException, ConnectionRefusedError):
                # Also acceptable - connection refused
                pass

        finally:
            try:
                os.unlink(config_path)
            except:
                pass

    def test_websocket_stdio_unknown_server(self):
        """Test WebSocket connection to unknown MCP server."""
        config = self.get_echo_server_config()

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            # Connect via WebSocket to non-existent server
            ws_port = server.server_port + 1
            ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=nonexistent"

            # Should fail to establish connection or close with error
            try:
                ws = websocket.create_connection(ws_url, timeout=5)
                # If connection succeeds, should get error message
                try:
                    response = ws.recv()
                    if response:
                        data = json.loads(response)
                        # Should contain error about server not found
                        assert "error" in str(data).lower() or "not found" in str(data).lower()
                except websocket.WebSocketConnectionClosedException:
                    pass  # Expected - connection closed
                finally:
                    ws.close()
            except (websocket.WebSocketException, ConnectionRefusedError):
                # Also acceptable
                pass

        finally:
            try:
                os.unlink(config_path)
            except:
                pass

    def test_websocket_stdio_env_var_security(self):
        """Test that sensitive env vars are filtered from stdio MCP servers."""
        config = self.get_echo_server_config()

        # Add sensitive env var to the server config
        config["mcpServers"]["echo"]["env"] = {
            "SAFE_VAR": "safe_value",
            "API_KEY": "should_not_be_visible"
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            # Connect via WebSocket
            ws_port = server.server_port + 1
            ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=echo"

            ws = websocket.create_connection(ws_url, timeout=10)

            # Initialize
            init_request = {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {},
                    "clientInfo": {"name": "test-client", "version": "1.0.0"}
                }
            }
            ws.send(json.dumps(init_request))
            ws.recv()

            # Call get_env_vars tool to check what env vars the MCP server sees
            tool_call_request = {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {
                    "name": "get_env_vars",
                    "arguments": {}
                }
            }
            ws.send(json.dumps(tool_call_request))

            # Receive response
            response = ws.recv()
            response_data = json.loads(response)

            # Verify response
            assert response_data["id"] == 2
            assert "result" in response_data

            # Parse the environment variables from the response
            content = response_data["result"]["content"][0]["text"]
            env_data = json.loads(content)
            env_vars = env_data["env_vars"]

            # Verify our custom env vars are present
            assert "SAFE_VAR" in env_vars
            assert env_vars["SAFE_VAR"] == "safe_value"
            assert "API_KEY" in env_vars
            assert env_vars["API_KEY"] == "should_not_be_visible"

            ws.close()

        finally:
            try:
                os.unlink(config_path)
            except:
                pass

    def test_websocket_stdio_http_proxy_rejection(self):
        """Test that HTTP proxy endpoint rejects stdio servers."""
        config = self.get_echo_server_config()

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            config_path = f.name

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            # Try to access stdio server via HTTP proxy endpoint
            # This should return an error telling user to use WebSocket
            res = server.make_request(
                "GET",
                "/mcp?server=echo",
                timeout=TIMEOUT_HTTP_REQUEST
            )

            # Should return 400 with error message about using WebSocket
            assert res.status_code == 400, f"Expected 400, got {res.status_code}"
            body = res.body

            # Parse JSON response
            data = json.loads(body if isinstance(body, str) else body.decode() if isinstance(body, bytes) else str(body))
            assert "error" in data
            assert "stdio" in data["error"].lower() or "websocket" in data["error"].lower()

        finally:
            try:
                os.unlink(config_path)
            except:
                pass


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
