#!/usr/bin/env node
/**
 * MCP WebSocket Connection Tests
 * Run with: node tools/server/tests/mcp-ws-test.mjs
 */

import WebSocket from 'ws';

const BASE_URL = process.env.MCP_WS_URL || 'ws://localhost:8081';
const MCP_SERVER = process.env.MCP_SERVER || 'python';
const TIMEOUT_MS = 10000;

const results = [];

async function test(name, fn) {
  const start = Date.now();
  try {
    await fn();
    results.push({ name, passed: true, duration: Date.now() - start });
    console.log(`✓ ${name} (${Date.now() - start}ms)`);
  } catch (err) {
    const error = err instanceof Error ? err.message : String(err);
    results.push({ name, passed: false, error, duration: Date.now() - start });
    console.log(`✗ ${name}: ${error}`);
  }
}

function createWebSocket(path) {
  return new Promise((resolve, reject) => {
    const url = `${BASE_URL}${path}`;
    console.log(`  Connecting to ${url}...`);
    const ws = new WebSocket(url);
    const timeout = setTimeout(() => {
      ws.close();
      reject(new Error(`Connection timeout after ${TIMEOUT_MS}ms`));
    }, TIMEOUT_MS);
    
    ws.on('open', () => {
      clearTimeout(timeout);
      resolve(ws);
    });
    ws.on('error', (err) => {
      clearTimeout(timeout);
      reject(err);
    });
  });
}

function sendAndReceive(ws, message, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      reject(new Error(`Response timeout after ${timeoutMs}ms`));
    }, timeoutMs);
    
    ws.once('message', (data) => {
      clearTimeout(timeout);
      try {
        resolve(JSON.parse(data.toString()));
      } catch {
        resolve(data.toString());
      }
    });
    
    ws.send(JSON.stringify(message));
  });
}

async function runTests() {
  console.log('\n=== MCP WebSocket Tests ===\n');
  
  // Test 1: Basic WebSocket connection to MCP endpoint
  await test(`Connect to /mcp?server=${MCP_SERVER}`, async () => {
    const ws = await createWebSocket(`/mcp?server=${MCP_SERVER}`);
    ws.close();
  });

  // Test 2: MCP Initialize handshake
  await test('MCP Initialize handshake', async () => {
    const ws = await createWebSocket(`/mcp?server=${MCP_SERVER}`);
    try {
      const initRequest = {
        jsonrpc: '2.0',
        id: 1,
        method: 'initialize',
        params: {
          protocolVersion: '2024-11-05',
          capabilities: {},
          clientInfo: { name: 'test-client', version: '1.0.0' }
        }
      };
      const response = await sendAndReceive(ws, initRequest);
      if (!response.result?.protocolVersion) {
        throw new Error(`Invalid init response: ${JSON.stringify(response)}`);
      }
    } finally {
      ws.close();
    }
  });

  // Test 3: List tools after init
  await test('List MCP tools', async () => {
    const ws = await createWebSocket(`/mcp?server=${MCP_SERVER}`);
    try {
      // Initialize first (may take time to start MCP process)
      await sendAndReceive(ws, {
        jsonrpc: '2.0',
        id: 1,
        method: 'initialize',
        params: {
          protocolVersion: '2024-11-05',
          capabilities: {},
          clientInfo: { name: 'test-client', version: '1.0.0' }
        }
      }, 15000);  // Longer timeout for process startup

      // Send initialized notification
      ws.send(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }));

      // List tools
      const response = await sendAndReceive(ws, {
        jsonrpc: '2.0',
        id: 2,
        method: 'tools/list'
      }, 10000);  // Longer timeout since init may take a while
      
      if (!response.result?.tools) {
        throw new Error(`Invalid tools response: ${JSON.stringify(response)}`);
      }
      console.log(`    Found ${response.result.tools.length} tools`);
    } finally {
      ws.close();
    }
  });

  // Test 4: Call a tool (if any tools are available)
  await test('Call first available tool', async () => {
    const ws = await createWebSocket(`/mcp?server=${MCP_SERVER}`);
    try {
      // Initialize
      await sendAndReceive(ws, {
        jsonrpc: '2.0',
        id: 1,
        method: 'initialize',
        params: {
          protocolVersion: '2024-11-05',
          capabilities: {},
          clientInfo: { name: 'test-client', version: '1.0.0' }
        }
      });

      ws.send(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }));

      // List tools first
      const toolsResponse = await sendAndReceive(ws, {
        jsonrpc: '2.0',
        id: 2,
        method: 'tools/list'
      });

      if (!toolsResponse.result?.tools?.length) {
        console.log('    No tools available, skipping tool call');
        return;
      }

      const firstTool = toolsResponse.result.tools[0];
      console.log(`    Calling tool: ${firstTool.name}`);

      // Call the first tool with empty args (may fail, but tests the flow)
      const response = await sendAndReceive(ws, {
        jsonrpc: '2.0',
        id: 3,
        method: 'tools/call',
        params: {
          name: firstTool.name,
          arguments: {}
        }
      }, 15000); // Longer timeout for tool execution

      // We just want to verify the response structure
      if (!response.jsonrpc) {
        throw new Error(`Invalid response: ${JSON.stringify(response)}`);
      }
      console.log(`    Tool response received: ${response.error ? 'error' : 'success'}`);
    } finally {
      ws.close();
    }
  });

  // Test 5: Reconnection after close
  await test('Reconnect after close', async () => {
    const ws1 = await createWebSocket(`/mcp?server=${MCP_SERVER}`);
    ws1.close();
    await new Promise(r => setTimeout(r, 100));
    const ws2 = await createWebSocket(`/mcp?server=${MCP_SERVER}`);
    ws2.close();
  });

  // Print summary
  console.log('\n=== Summary ===');
  const passed = results.filter(r => r.passed).length;
  const failed = results.filter(r => !r.passed).length;
  console.log(`Passed: ${passed}, Failed: ${failed}`);
  
  if (failed > 0) {
    console.log('\nFailed tests:');
    results.filter(r => !r.passed).forEach(r => {
      console.log(`  - ${r.name}: ${r.error}`);
    });
    process.exit(1);
  }
}

runTests().catch(err => {
  console.error('Test runner error:', err);
  process.exit(1);
});
