#!/usr/bin/env python3
"""
Simple MCP (Model Context Protocol) server for testing.

This server implements the MCP JSON-RPC protocol over stdio and provides
two tools:
- echo: Returns the input message
- get_env_vars: Returns all environment variables (for security testing)

Usage:
    python mcp_echo_server.py

The server reads JSON-RPC messages from stdin (one per line) and writes
responses to stdout. It follows the MCP specification for initialize,
tools/list, and tools/call methods.
"""

import json
import os
import sys


def handle_initialize(req_id: int) -> dict:
    """Handle MCP initialize request."""
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "result": {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "test-echo", "version": "1.0.0"}
        }
    }


def handle_tools_list(req_id: int) -> dict:
    """Handle MCP tools/list request."""
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "result": {
            "tools": [
                {
                    "name": "echo",
                    "description": "Echo back the input message",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "message": {
                                "type": "string",
                                "description": "The message to echo back"
                            }
                        },
                        "required": ["message"]
                    }
                },
                {
                    "name": "get_env_vars",
                    "description": "Return all environment variables visible to this process",
                    "inputSchema": {
                        "type": "object",
                        "properties": {},
                        "required": []
                    }
                }
            ]
        }
    }


def handle_tool_call(req_id: int, tool_name: str, arguments: dict) -> dict:
    """Handle MCP tools/call request."""
    if tool_name == "echo":
        message = arguments.get("message", "")
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "content": [{"type": "text", "text": message}]
            }
        }
    elif tool_name == "get_env_vars":
        # Return environment variables as structured content
        # This is used for security testing to verify env var filtering
        env_vars = dict(os.environ)
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "content": [
                    {
                        "type": "text",
                        "text": json.dumps({"env_vars": env_vars}, indent=2)
                    }
                ],
                "structuredContent": {"env_vars": env_vars}
            }
        }
    else:
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {"code": -32601, "message": f"Unknown tool: {tool_name}"}
        }


def handle_request(request: dict) -> dict | None:
    """Process a single JSON-RPC request and return response (or None for notifications)."""
    method = request.get("method", "")
    req_id = request.get("id")

    # Notifications (no id) should not get a response
    if req_id is None or method.startswith("notifications/"):
        return None

    if method == "initialize":
        return handle_initialize(req_id)
    elif method == "tools/list":
        return handle_tools_list(req_id)
    elif method == "tools/call":
        params = request.get("params", {})
        tool_name = params.get("name", "")
        arguments = params.get("arguments", {})
        return handle_tool_call(req_id, tool_name, arguments)
    else:
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {"code": -32601, "message": f"Method not found: {method}"}
        }


def main():
    """Main loop: read JSON-RPC messages from stdin, respond on stdout."""
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            request = json.loads(line)
            response = handle_request(request)

            if response is not None:
                print(json.dumps(response), flush=True)

        except json.JSONDecodeError as e:
            # Parse error
            error_response = {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32700, "message": f"Parse error: {e}"}
            }
            print(json.dumps(error_response), flush=True)

        except Exception as e:
            # Internal error - try to send error response if we have an id
            req_id = None
            try:
                req_id = json.loads(line).get("id")
            except:
                pass

            if req_id is not None:
                error_response = {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "error": {"code": -32603, "message": f"Internal error: {e}"}
                }
                print(json.dumps(error_response), flush=True)


if __name__ == "__main__":
    main()
