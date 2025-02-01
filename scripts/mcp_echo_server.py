#!/usr/bin/env python3

import sys
import json

# Force stdout to be unbuffered
sys.stdout.reconfigure(line_buffering=True)

print("JSON-RPC server starting...", file=sys.stderr, flush=True)

for line in sys.stdin:
    line = line.strip()
    if not line:
        print("Received blank line", file=sys.stderr, flush=True)
        continue  # ignore blank lines
        
    print(f"Received request: {line}", file=sys.stderr, flush=True)
    
    try:
        request = json.loads(line)
        # Respond with a trivial "echo" result
        response = {
            "jsonrpc": "2.0",
            "id": request.get("id"),
            "result": {
                "echoedMethod": request.get("method"),
                "echoedParams": request.get("params"),
            }
        }
        response_str = json.dumps(response)
        print(f"Sending response: {response_str}", file=sys.stderr, flush=True)
        print(response_str, flush=True)  # Make sure to flush the response
        
    except json.JSONDecodeError as e:
        error_response = {
            "jsonrpc": "2.0",
            "error": {
                "code": -32700,
                "message": f"JSON parse error: {str(e)}"
            },
            "id": None
        }
        print(f"Sending error: {error_response}", file=sys.stderr, flush=True)
        print(json.dumps(error_response), flush=True)

print("Server exiting", file=sys.stderr, flush=True)