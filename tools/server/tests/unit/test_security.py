import pytest
from openai import OpenAI
from utils import *

# Raw websocket for authentication tests
import websocket

server = ServerPreset.tinyllama2()

TEST_API_KEY = "sk-this-is-the-secret-key"

@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.tinyllama2()
    server.api_key = TEST_API_KEY


@pytest.mark.parametrize("endpoint", ["/health", "/models"])
def test_access_public_endpoint(endpoint: str):
    global server
    server.start()
    res = server.make_request("GET", endpoint)
    assert res.status_code == 200
    assert "error" not in res.body


@pytest.mark.parametrize("api_key", [None, "invalid-key"])
def test_incorrect_api_key(api_key: str):
    global server
    server.start()
    res = server.make_request("POST", "/completions", data={
        "prompt": "I believe the meaning of life is",
    }, headers={
        "Authorization": f"Bearer {api_key}" if api_key else None,
    })
    assert res.status_code == 401
    assert "error" in res.body
    assert res.body["error"]["type"] == "authentication_error"


def test_correct_api_key():
    global server
    server.start()
    res = server.make_request("POST", "/completions", data={
        "prompt": "I believe the meaning of life is",
    }, headers={
        "Authorization": f"Bearer {TEST_API_KEY}",
    })
    assert res.status_code == 200
    assert "error" not in res.body
    assert "content" in res.body


def test_correct_api_key_anthropic_header():
    global server
    server.start()
    res = server.make_request("POST", "/completions", data={
        "prompt": "I believe the meaning of life is",
    }, headers={
        "X-Api-Key": TEST_API_KEY,
    })
    assert res.status_code == 200
    assert "error" not in res.body
    assert "content" in res.body


def test_openai_library_correct_api_key():
    global server
    server.start()
    client = OpenAI(api_key=TEST_API_KEY, base_url=f"http://{server.server_host}:{server.server_port}")
    res = client.chat.completions.create(
        model="gpt-3.5-turbo",
        messages=[
            {"role": "system", "content": "You are a chatbot."},
            {"role": "user", "content": "What is the meaning of life?"},
        ],
    )
    assert len(res.choices) == 1


@pytest.mark.parametrize("origin,cors_header,cors_header_value", [
    ("localhost", "Access-Control-Allow-Origin", "localhost"),
    ("web.mydomain.fr", "Access-Control-Allow-Origin", "web.mydomain.fr"),
    ("origin", "Access-Control-Allow-Credentials", "true"),
    ("web.mydomain.fr", "Access-Control-Allow-Methods", "GET, POST"),
    ("web.mydomain.fr", "Access-Control-Allow-Headers", "*"),
])
def test_cors_options(origin: str, cors_header: str, cors_header_value: str):
    global server
    server.start()
    res = server.make_request("OPTIONS", "/completions", headers={
        "Origin": origin,
        "Access-Control-Request-Method": "POST",
        "Access-Control-Request-Headers": "Authorization",
    })
    assert res.status_code == 200
    assert cors_header in res.headers
    assert res.headers[cors_header] == cors_header_value


@pytest.mark.parametrize(
    "media_path, image_url, success",
    [
        (None,             "file://mtmd/test-1.jpeg",    False), # disabled media path, should fail
        ("../../../tools", "file://mtmd/test-1.jpeg",    True),
        ("../../../tools", "file:////mtmd//test-1.jpeg", True),  # should be the same file as above
        ("../../../tools", "file://mtmd/notfound.jpeg",  False), # non-existent file
        ("../../../tools", "file://../mtmd/test-1.jpeg", False), # no directory traversal
    ]
)
def test_local_media_file(media_path, image_url, success,):
    server = ServerPreset.tinygemma3()
    server.media_path = media_path
    server.start()
    res = server.make_request("POST", "/chat/completions", data={
        "max_tokens": 1,
        "messages": [
            {"role": "user", "content": [
                {"type": "text", "text": "test"},
                {"type": "image_url", "image_url": {
                    "url": image_url,
                }},
            ]},
        ],
    })
    if success:
        assert res.status_code == 200
    else:
        assert res.status_code == 400


class TestWebSocketAuthentication:
    """Test WebSocket authentication with API keys."""

    @pytest.mark.parametrize("api_key,should_succeed", [
        (None, False),           # No API key when one is required
        ("invalid-key", False),  # Wrong API key
        ("sk-this-is-the-secret-key", True),  # Correct API key
    ])
    def test_websocket_api_key(self, api_key, should_succeed):
        """Test WebSocket connection with various API keys."""
        server = ServerPreset.tinyllama2()
        server.api_key = TEST_API_KEY
        server.webui_mcp = True  # Enable WebSocket/MCP support
        server.start()

        ws_port = server.server_port + 1
        # Note: we don't include server parameter since we're testing auth at the
        # WebSocket handshake level, which happens before MCP routing
        ws_url = f"ws://{server.server_host}:{ws_port}/mcp"

        # Prepare headers with Authorization if api_key is provided
        headers = {}
        if api_key is not None:
            headers["Authorization"] = f"Bearer {api_key}"

        try:
            ws = websocket.create_connection(ws_url, timeout=5, header=headers)
            # If we get here, connection succeeded
            # For valid auth, connection should open but may close due to missing server param
            # For invalid auth, connection should fail at handshake
            ws.close()
            assert should_succeed, "Expected connection to fail but it succeeded"
        except websocket.WebSocketBadStatusException as e:
            # Connection failed with HTTP error (401 for invalid auth)
            assert not should_succeed, f"Expected connection to succeed but got: {e}"
            assert e.status_code == 401, f"Expected 401, got {e.status_code}"
        except (websocket.WebSocketException, ConnectionRefusedError) as e:
            # Other connection failures
            assert not should_succeed, f"Expected connection to succeed but got: {e}"

    def test_websocket_no_auth_required(self):
        """Test WebSocket connection when no API key is configured."""
        server = ServerPreset.tinyllama2()
        server.api_key = None  # No API key required
        server.webui_mcp = True  # Enable WebSocket/MCP support
        server.start()

        ws_port = server.server_port + 1
        ws_url = f"ws://{server.server_host}:{ws_port}/mcp"

        # Should connect successfully without any auth
        # The connection may close due to missing server param, but auth should pass
        ws = websocket.create_connection(ws_url, timeout=5)
        ws.close()
