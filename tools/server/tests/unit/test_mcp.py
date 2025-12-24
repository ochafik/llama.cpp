#!/usr/bin/env python
"""
Tests for MCP (Model Context Protocol) server functionality.

Tests cover:
- HTTP endpoint: /mcp/servers
- WebSocket connection lifecycle (on HTTP port + 1)
- MCP JSON-RPC protocol (using official MCP SDK)
- Environment variable filtering (security)
"""

import pytest
import pytest_asyncio
import asyncio
import json
import os
import tempfile
from pathlib import Path
import sys

# Ensure parent path is in sys.path
path = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(path))

from utils import ServerProcess

# Raw websocket for low-level protocol tests
import websocket

# Official MCP SDK for protocol-level tests
from mcp.client.websocket import websocket_client
from mcp.client.session import ClientSession

server: ServerProcess

TIMEOUT_SERVER_START = 60
TIMEOUT_HTTP_REQUEST = 10
TIMEOUT_WS = 5

# Environment variables that are allowed to be inherited by MCP subprocesses
# Must match MCP_INHERITED_ENV_VARS in server-mcp-bridge.cpp
if sys.platform == "win32":
    MCP_ALLOWED_ENV_VARS = {
        "APPDATA", "HOMEDRIVE", "HOMEPATH", "LOCALAPPDATA", "PATH",
        "PROCESSOR_ARCHITECTURE", "SYSTEMDRIVE", "SYSTEMROOT", "TEMP",
        "USERNAME", "USERPROFILE", "PROGRAMFILES"
    }
else:
    MCP_ALLOWED_ENV_VARS = {"HOME", "LOGNAME", "PATH", "SHELL", "TERM", "USER"}


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
    server.webui_mcp = True  # Enable MCP WebSocket support
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


class TestMcpHttpEndpoints:
    """Test MCP HTTP endpoints."""

    def test_mcp_servers_endpoint_no_config(self):
        """Test /mcp/servers returns empty list when no config is set."""
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        res = server.make_request("GET", "/mcp/servers", timeout=TIMEOUT_HTTP_REQUEST)
        assert res.status_code == 200, f"Expected 200, got {res.status_code}"

        body = res.body
        assert "servers" in body, f"Expected 'servers' key in response: {body}"
        # Without config, should return empty list or available servers
        assert isinstance(body["servers"], list), f"Expected list, got {type(body['servers'])}"



class TestMcpWithConfig:
    """Test MCP with a configuration file."""

    @pytest.fixture
    def mcp_config_file(self):
        """Create a temporary MCP config file."""
        config = {
            "mcpServers": {
                "echo-test": {
                    "command": "echo",
                    "args": ["test"],
                    "env": {}
                }
            }
        }

        with tempfile.NamedTemporaryFile(
            mode='w',
            suffix='.json',
            delete=False
        ) as f:
            json.dump(config, f)
            config_path = f.name

        yield config_path

        # Cleanup
        try:
            os.unlink(config_path)
        except:
            pass

    def test_mcp_servers_with_config(self, mcp_config_file):
        """Test /mcp/servers returns configured servers."""
        server.mcp_config = mcp_config_file
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        res = server.make_request("GET", "/mcp/servers", timeout=TIMEOUT_HTTP_REQUEST)
        assert res.status_code == 200, f"Expected 200, got {res.status_code}"

        body = res.body
        assert "servers" in body, f"Expected 'servers' key in response: {body}"
        servers = body["servers"]

        # Should have the echo-test server (servers is a list of objects with 'name' key)
        server_names = [s["name"] if isinstance(s, dict) else s for s in servers]
        assert "echo-test" in server_names, f"Expected 'echo-test' in servers: {servers}"


class TestMcpWebSocket:
    """Test MCP WebSocket functionality."""

    def test_websocket_connection_without_server_param(self):
        """Test WebSocket connection fails without server parameter."""
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        ws_port = server.server_port + 1
        ws_url = f"ws://{server.server_host}:{ws_port}/mcp"

        # Server should close connection immediately when no server parameter is provided
        # This can manifest as connection closed, bad status, timeout, or empty message
        try:
            ws = websocket.create_connection(ws_url, timeout=TIMEOUT_WS)
            try:
                # Try to receive - should fail since server closes connection
                result = ws.recv()
                # Empty result or connection close is expected
                if result:
                    data = json.loads(result)
                    assert "error" in data or data.get("error") is not None, \
                        f"Expected error response without server param: {data}"
                # Empty result means connection was closed - this is expected
            except (websocket.WebSocketConnectionClosedException, websocket.WebSocketTimeoutException):
                # This is expected - server closes connection without valid server param
                pass
            finally:
                ws.close()
        except (websocket.WebSocketBadStatusException, websocket.WebSocketException) as e:
            # Also acceptable - connection may be rejected during handshake
            pass

    def test_websocket_connection_invalid_server(self):
        """Test WebSocket connection with invalid server name returns error on message."""
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        ws_port = server.server_port + 1
        ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=nonexistent"

        ws = websocket.create_connection(ws_url, timeout=TIMEOUT_WS)
        try:
            # Connection opens successfully, but sending a message should fail
            # because the server config doesn't exist
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

            # Should get error response about unknown server
            result = ws.recv()
            data = json.loads(result)
            assert "error" in data, \
                f"Expected error for invalid server: {data}"
            # Error should indicate MCP process not available
            assert data["error"]["code"] == -32000 or "not available" in data["error"]["message"].lower(), \
                f"Expected 'MCP process not available' error: {data}"
        except websocket.WebSocketConnectionClosedException:
            # Also acceptable - server may close connection
            pass
        finally:
            ws.close()


def get_mcp_echo_script_path():
    """Get the path to the MCP echo server fixture script."""
    return str(Path(__file__).parent.parent / "fixtures" / "mcp_echo_server.py")


@pytest.fixture
def python_mcp_config():
    """Create config with a Python-based echo MCP server for testing."""
    script_path = get_mcp_echo_script_path()

    # Create config pointing to the script
    config = {
        "mcpServers": {
            "test-echo": {
                "command": sys.executable,
                "args": [script_path],
                "env": {"PYTHONUNBUFFERED": "1"}
            }
        }
    }

    with tempfile.NamedTemporaryFile(
        mode='w',
        suffix='.json',
        delete=False
    ) as config_file:
        json.dump(config, config_file)
        config_path = config_file.name

    yield config_path

    # Cleanup config file (script is a fixture, not temp)
    try:
        os.unlink(config_path)
    except:
        pass


class TestMcpJsonRpcProtocol:
    """Test MCP JSON-RPC protocol handling using the official MCP SDK."""

    @pytest.mark.asyncio
    async def test_mcp_initialize_and_list_tools(self, python_mcp_config):
        """Test MCP initialize handshake and tools/list using the MCP SDK."""
        server.mcp_config = python_mcp_config
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        ws_port = server.server_port + 1
        ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=test-echo"

        async with websocket_client(ws_url) as streams:
            read_stream, write_stream = streams
            async with ClientSession(read_stream, write_stream) as session:
                # Initialize (handled by ClientSession automatically)
                result = await session.initialize()

                # Verify initialization result
                assert result.protocolVersion is not None, \
                    f"Expected protocolVersion in result: {result}"

                # List tools
                tools_result = await session.list_tools()
                tools = tools_result.tools

                assert len(tools) >= 2, \
                    f"Expected at least two tools (echo, get_env_vars): {tools}"
                tool_names = [t.name for t in tools]
                assert "echo" in tool_names, f"Expected echo tool: {tools}"
                assert "get_env_vars" in tool_names, f"Expected get_env_vars tool: {tools}"

    @pytest.mark.asyncio
    async def test_mcp_tool_call(self, python_mcp_config):
        """Test MCP tools/call method using the MCP SDK."""
        server.mcp_config = python_mcp_config
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        ws_port = server.server_port + 1
        ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=test-echo"

        async with websocket_client(ws_url) as streams:
            read_stream, write_stream = streams
            async with ClientSession(read_stream, write_stream) as session:
                # Initialize
                await session.initialize()

                # Call the echo tool
                result = await session.call_tool("echo", {"message": "Hello, MCP!"})

                # Verify result
                assert result.content is not None, f"Expected content: {result}"
                assert len(result.content) > 0, f"Expected content items: {result}"

                # Extract text from first content item
                first_content = result.content[0]
                assert hasattr(first_content, 'text'), f"Expected text content: {first_content}"
                assert first_content.text == "Hello, MCP!", \
                    f"Expected echoed message: {first_content.text}"


class TestMcpJsonRpcProtocolRaw:
    """Test MCP JSON-RPC protocol handling using raw WebSocket (for edge cases)."""

    def test_mcp_initialize_handshake_raw(self, python_mcp_config):
        """Test MCP initialize handshake via raw WebSocket."""
        server.mcp_config = python_mcp_config
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        ws_port = server.server_port + 1
        ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=test-echo"

        ws = websocket.create_connection(ws_url, timeout=TIMEOUT_WS)
        try:
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
            result = ws.recv()
            response = json.loads(result)

            assert response.get("id") == 1, f"Expected id=1: {response}"
            assert "result" in response, f"Expected result in response: {response}"
            assert "protocolVersion" in response["result"], \
                f"Expected protocolVersion in result: {response}"

        finally:
            ws.close()

    def test_mcp_tools_list_raw(self, python_mcp_config):
        """Test MCP tools/list method via raw WebSocket."""
        server.mcp_config = python_mcp_config
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

        ws_port = server.server_port + 1
        ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=test-echo"

        ws = websocket.create_connection(ws_url, timeout=TIMEOUT_WS)
        try:
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
            ws.recv()  # Consume init response

            # Send initialized notification
            ws.send(json.dumps({
                "jsonrpc": "2.0",
                "method": "notifications/initialized"
            }))

            # Request tools list
            tools_request = {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/list",
                "params": {}
            }
            ws.send(json.dumps(tools_request))

            result = ws.recv()
            response = json.loads(result)

            assert response.get("id") == 2, f"Expected id=2: {response}"
            assert "result" in response, f"Expected result: {response}"
            assert "tools" in response["result"], f"Expected tools: {response}"

            tools = response["result"]["tools"]
            assert len(tools) >= 2, f"Expected at least two tools (echo, get_env_vars): {tools}"
            tool_names = [t["name"] for t in tools]
            assert "echo" in tool_names, f"Expected echo tool: {tools}"
            assert "get_env_vars" in tool_names, f"Expected get_env_vars tool: {tools}"

        finally:
            ws.close()


@pytest.fixture
def python_mcp_config_with_secret():
    """Create config with a Python-based MCP server and config-specified env var."""
    script_path = get_mcp_echo_script_path()

    # Create config pointing to the script
    config = {
        "mcpServers": {
            "test-echo": {
                "command": sys.executable,
                "args": [script_path],
                "env": {
                    "PYTHONUNBUFFERED": "1",
                    "MCP_CONFIG_VAR": "config_value"  # This should be passed
                }
            }
        }
    }

    with tempfile.NamedTemporaryFile(
        mode='w',
        suffix='.json',
        delete=False
    ) as config_file:
        json.dump(config, config_file)
        config_path = config_file.name

    yield config_path

    # Cleanup config file (script is a fixture, not temp)
    try:
        os.unlink(config_path)
    except:
        pass


class TestMcpEnvVarFiltering:
    """Test that MCP subprocess only receives vetted environment variables."""

    @pytest.mark.asyncio
    async def test_env_vars_filtering(self, python_mcp_config_with_secret):
        """Test that MCP subprocess only receives allowed env vars using MCP SDK."""
        config_path = python_mcp_config_with_secret

        # Set some secret env vars that should NOT be passed to subprocess
        secret_vars = {
            "SECRET_API_KEY": "super_secret_key_12345",
            "AWS_SECRET_ACCESS_KEY": "aws_secret_12345",
            "DATABASE_PASSWORD": "db_password_12345",
        }
        old_env = {}
        for key, value in secret_vars.items():
            old_env[key] = os.environ.get(key)
            os.environ[key] = value

        try:
            server.mcp_config = config_path
            server.start(timeout_seconds=TIMEOUT_SERVER_START)

            ws_port = server.server_port + 1
            ws_url = f"ws://{server.server_host}:{ws_port}/mcp?server=test-echo"

            async with websocket_client(ws_url) as streams:
                read_stream, write_stream = streams
                async with ClientSession(read_stream, write_stream) as session:
                    # Initialize
                    await session.initialize()

                    # Call get_env_vars tool to see what env vars the subprocess received
                    result = await session.call_tool("get_env_vars", {})

                    # Extract env vars from the result
                    subprocess_env = {}
                    if result.content:
                        first_content = result.content[0]
                        if hasattr(first_content, 'text'):
                            text_data = json.loads(first_content.text)
                            subprocess_env = text_data.get("env_vars", {})

                    assert subprocess_env, f"Could not extract env vars from response: {result}"

                    # Verify secret vars are NOT present
                    for secret_key in secret_vars:
                        assert secret_key not in subprocess_env, \
                            f"Secret env var '{secret_key}' was leaked to MCP subprocess! " \
                            f"Subprocess env: {list(subprocess_env.keys())}"

                    # Verify allowed vars are present (at least some of them)
                    present_allowed = [v for v in MCP_ALLOWED_ENV_VARS if v in subprocess_env]
                    assert len(present_allowed) > 0, \
                        f"None of the allowed env vars {MCP_ALLOWED_ENV_VARS} are present. " \
                        f"Subprocess env: {list(subprocess_env.keys())}"

                    # Verify config-specified env vars ARE present
                    assert "PYTHONUNBUFFERED" in subprocess_env, \
                        f"Config-specified PYTHONUNBUFFERED not in env: {list(subprocess_env.keys())}"
                    assert "MCP_CONFIG_VAR" in subprocess_env, \
                        f"Config-specified MCP_CONFIG_VAR not in env: {list(subprocess_env.keys())}"
                    assert subprocess_env["MCP_CONFIG_VAR"] == "config_value", \
                        f"MCP_CONFIG_VAR has wrong value: {subprocess_env['MCP_CONFIG_VAR']}"

                    # Verify PATH is present (essential for finding executables)
                    assert "PATH" in subprocess_env, \
                        f"PATH should be inherited: {list(subprocess_env.keys())}"

        finally:
            # Restore environment
            for key, value in old_env.items():
                if value is None:
                    if key in os.environ:
                        del os.environ[key]
                else:
                    os.environ[key] = value


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
