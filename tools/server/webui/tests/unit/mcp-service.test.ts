import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { McpService } from '$lib/services/mcp';

// Mock the MCP SDK transports
vi.mock('@modelcontextprotocol/sdk/client/websocket.js', () => ({
	WebSocketClientTransport: vi.fn().mockImplementation((url: URL) => ({
		url,
		onclose: undefined,
		onerror: undefined,
		onmessage: undefined,
		start: vi.fn(),
		close: vi.fn(),
		send: vi.fn()
	}))
}));

vi.mock('@modelcontextprotocol/sdk/client/streamableHttp.js', () => ({
	StreamableHTTPClientTransport: vi.fn().mockImplementation((url: URL) => ({
		url,
		onclose: undefined,
		onerror: undefined,
		onmessage: undefined,
		start: vi.fn(),
		close: vi.fn(),
		send: vi.fn()
	}))
}));

vi.mock('@modelcontextprotocol/sdk/client/index.js', () => ({
	Client: vi.fn().mockImplementation(() => ({
		connect: vi.fn(),
		close: vi.fn(),
		listTools: vi.fn().mockResolvedValue({ tools: [] }),
		callTool: vi.fn(),
		ping: vi.fn()
	}))
}));

describe('McpService transport selection', () => {
	let fetchMock: ReturnType<typeof vi.fn>;

	beforeEach(() => {
		// Mock window.location
		const mockLocation = {
			hostname: 'localhost',
			port: '8080',
			protocol: 'http:',
			origin: 'http://localhost:8080'
		} as Location;

		// @ts-expect-error - Mocking global object
		global.window = {
			location: mockLocation
		};

		// Mock fetch
		fetchMock = vi.fn();
		global.fetch = fetchMock;
	});

	afterEach(() => {
		vi.clearAllMocks();
	});

	it('should use WebSocketClientTransport for stdio servers', async () => {
		const serverName = 'test-stdio-server';
		const service = new McpService(serverName);

		// Mock the /mcp/servers response to return stdio type
		fetchMock.mockResolvedValueOnce({
			ok: true,
			json: async () => ({
				servers: [{ name: serverName, type: 'stdio' }]
			})
		});

		// Mock Client.connect to prevent actual connection
		const { Client } = await import('@modelcontextprotocol/sdk/client/index.js');
		const mockClient = new Client({ name: 'test', version: '1.0.0' }, { capabilities: {} });
		vi.mocked(mockClient.connect).mockResolvedValue();

		try {
			await service.connect();
		} catch {
			// Ignore connection errors for this test
		}

		// Verify fetch was called with correct URL
		expect(fetchMock).toHaveBeenCalledWith('/mcp/servers');

		// Verify WebSocketClientTransport was instantiated with correct URL
		const { WebSocketClientTransport } = await import(
			'@modelcontextprotocol/sdk/client/websocket.js'
		);
		expect(WebSocketClientTransport).toHaveBeenCalledWith(
			new URL(`ws://localhost:8081/mcp?server=${encodeURIComponent(serverName)}`)
		);
	});

	it('should use StreamableHTTPClientTransport for http servers', async () => {
		const serverName = 'test-http-server';
		const service = new McpService(serverName);

		// Mock the /mcp/servers response to return http type
		fetchMock.mockResolvedValueOnce({
			ok: true,
			json: async () => ({
				servers: [{ name: serverName, type: 'http' }]
			})
		});

		// Mock Client.connect to prevent actual connection
		const { Client } = await import('@modelcontextprotocol/sdk/client/index.js');
		const mockClient = new Client({ name: 'test', version: '1.0.0' }, { capabilities: {} });
		vi.mocked(mockClient.connect).mockResolvedValue();

		try {
			await service.connect();
		} catch {
			// Ignore connection errors for this test
		}

		// Verify fetch was called with correct URL
		expect(fetchMock).toHaveBeenCalledWith('/mcp/servers');

		// Verify StreamableHTTPClientTransport was instantiated with correct URL
		const { StreamableHTTPClientTransport } = await import(
			'@modelcontextprotocol/sdk/client/streamableHttp.js'
		);
		expect(StreamableHTTPClientTransport).toHaveBeenCalledWith(
			new URL(`http://localhost:8080/mcp?server=${encodeURIComponent(serverName)}`)
		);
	});

	it('should use wss:// protocol for HTTPS connections with stdio servers', async () => {
		// Update location to use HTTPS
		// @ts-expect-error - Mocking global object
		global.window = {
			location: {
				hostname: 'localhost',
				port: '8080',
				protocol: 'https:',
				origin: 'https://localhost:8080'
			} as Location
		};

		const serverName = 'test-stdio-server';
		const service = new McpService(serverName);

		// Mock the /mcp/servers response to return stdio type
		fetchMock.mockResolvedValueOnce({
			ok: true,
			json: async () => ({
				servers: [{ name: serverName, type: 'stdio' }]
			})
		});

		try {
			await service.connect();
		} catch {
			// Ignore connection errors for this test
		}

		// Verify WebSocketClientTransport was instantiated with wss:// URL
		const { WebSocketClientTransport } = await import(
			'@modelcontextprotocol/sdk/client/websocket.js'
		);
		expect(WebSocketClientTransport).toHaveBeenCalledWith(
			new URL(`wss://localhost:8081/mcp?server=${encodeURIComponent(serverName)}`)
		);
	});

	it('should throw error if server not found', async () => {
		const serverName = 'non-existent-server';
		const service = new McpService(serverName);

		// Mock the /mcp/servers response without the requested server
		fetchMock.mockResolvedValueOnce({
			ok: true,
			json: async () => ({
				servers: [{ name: 'other-server', type: 'stdio' }]
			})
		});

		await expect(service.connect()).rejects.toThrow(`MCP server not found: ${serverName}`);
	});

	it('should throw error if /mcp/servers request fails', async () => {
		const serverName = 'test-server';
		const service = new McpService(serverName);

		// Mock the /mcp/servers response to fail
		fetchMock.mockResolvedValueOnce({
			ok: false,
			statusText: 'Internal Server Error'
		});

		await expect(service.connect()).rejects.toThrow(
			'Failed to fetch MCP servers: Internal Server Error'
		);
	});

	it('should throw error for unknown server type', async () => {
		const serverName = 'test-unknown-server';
		const service = new McpService(serverName);

		// Mock the /mcp/servers response with unknown type
		fetchMock.mockResolvedValueOnce({
			ok: true,
			json: async () => ({
				servers: [{ name: serverName, type: 'unknown' }]
			})
		});

		await expect(service.connect()).rejects.toThrow('Unknown server type: unknown');
	});

	it('should use default port 80 when window.location.port is empty', async () => {
		// Update location to have no port (default HTTP port)
		// @ts-expect-error - Mocking global object
		global.window = {
			location: {
				hostname: 'localhost',
				port: '',
				protocol: 'http:',
				origin: 'http://localhost'
			} as Location
		};

		const serverName = 'test-stdio-server';
		const service = new McpService(serverName);

		// Mock the /mcp/servers response to return stdio type
		fetchMock.mockResolvedValueOnce({
			ok: true,
			json: async () => ({
				servers: [{ name: serverName, type: 'stdio' }]
			})
		});

		try {
			await service.connect();
		} catch {
			// Ignore connection errors for this test
		}

		// Verify WebSocketClientTransport was instantiated with port 81 (80 + 1)
		const { WebSocketClientTransport } = await import(
			'@modelcontextprotocol/sdk/client/websocket.js'
		);
		expect(WebSocketClientTransport).toHaveBeenCalledWith(
			new URL(`ws://localhost:81/mcp?server=${encodeURIComponent(serverName)}`)
		);
	});
});
