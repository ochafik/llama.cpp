#!/usr/bin/env python
"""
Tests for MCP (Model Context Protocol) HTTP proxy functionality.

Tests cover:
- HTTP endpoint: /mcp?server=... (GET and POST)
- CORS headers for browser compatibility
"""

import pytest
import json
import os
import tempfile
from pathlib import Path
import sys

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


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
