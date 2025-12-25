/**
 * MCP Service - Manages connections to MCP servers using the official MCP SDK
 *
 * Handles communication with Model Context Protocol servers using hybrid transport:
 * - stdio servers: WebSocketClientTransport (port + 1)
 * - HTTP servers: StreamableHTTPClientTransport (HTTP proxy)
 * Each MCP server gets its own connection.
 */

import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import { WebSocketClientTransport } from '@modelcontextprotocol/sdk/client/websocket.js';
import type { Tool, Notification } from '@modelcontextprotocol/sdk/types.js';

// Timeout constants - increased for Docker containers that may take time to start
const REQUEST_TIMEOUT_MS = 120_000; // 2 minutes for slow-starting containers
const MAX_RECONNECT_ATTEMPTS = 10; // More attempts for reliability
const MAX_RECONNECT_DELAY_MS = 60_000; // Max 60s between reconnect attempts
const INITIAL_RECONNECT_DELAY_MS = 1000; // Start with 1s delay

export class McpService {
	private client: Client | null = null;
	private transport: StreamableHTTPClientTransport | WebSocketClientTransport | null = null;
	private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
	private reconnectAttempts = 0;
	private manualDisconnect = false;
	private connectionGeneration = 0;

	// Event callbacks
	onToolsChanged?: (tools: Tool[]) => void;
	onNotification?: (notification: Notification) => void;
	onClose?: () => void;
	onError?: (error: Error) => void;
	onOpen?: () => void;

	constructor(public readonly serverName: string) {}

	/**
	 * Get the current connection generation (for preventing stale callbacks)
	 */
	getConnectionGeneration(): number {
		return this.connectionGeneration;
	}

	/**
	 * Increment the connection generation
	 */
	incrementConnectionGeneration(): number {
		return ++this.connectionGeneration;
	}

	/**
	 * Get server information from the backend
	 * Returns the server type (stdio or http) which determines the transport to use
	 */
	private async getServerInfo(): Promise<{ type: 'stdio' | 'http' }> {
		const res = await fetch('/mcp/servers');
		if (!res.ok) {
			throw new Error(`Failed to fetch MCP servers: ${res.statusText}`);
		}
		const data = await res.json();
		const server = data.servers.find((s: { name: string }) => s.name === this.serverName);
		if (!server) {
			throw new Error(`MCP server not found: ${this.serverName}`);
		}
		return server;
	}

	/**
	 * Connect to the MCP server using appropriate transport based on server type
	 *
	 * For stdio servers: Uses WebSocket on port + 1 (e.g., if HTTP is on 8080, WebSocket is on 8081)
	 * For remote HTTP servers: Uses HTTP proxy at /mcp?server={serverName} with CORS support
	 */
	async connect(): Promise<void> {
		try {
			// Get server information to determine transport type
			const serverInfo = await this.getServerInfo();

			// Select transport based on server type
			if (serverInfo.type === 'stdio') {
				// WebSocket for local stdio servers (port + 1)
				const wsPort = (parseInt(window.location.port) || 80) + 1;
				const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
				const wsUrl = `${wsProtocol}//${window.location.hostname}:${wsPort}/mcp?server=${encodeURIComponent(this.serverName)}`;
				console.log(`[MCP] Connecting to stdio server via WebSocket: ${wsUrl}`);
				this.transport = new WebSocketClientTransport(new URL(wsUrl));
			} else if (serverInfo.type === 'http') {
				// HTTP for remote servers (same port)
				const httpUrl = `${window.location.origin}/mcp?server=${encodeURIComponent(this.serverName)}`;
				console.log(`[MCP] Connecting to HTTP server via proxy: ${httpUrl}`);
				this.transport = new StreamableHTTPClientTransport(new URL(httpUrl));
			} else {
				throw new Error(`Unknown server type: ${serverInfo.type}`);
			}

			// Set up transport event handlers
			this.transport.onclose = () => {
				// Clear transport and client references so isConnected() returns false
				this.transport = null;
				this.client = null;
				this.onClose?.();
				// Only auto-reconnect if not manually disconnected
				if (!this.manualDisconnect) {
					this.scheduleReconnect();
				}
				this.manualDisconnect = false; // Reset for next connection
			};

			this.transport.onerror = (error: Error) => {
				console.error(`[MCP] Transport error for ${this.serverName}:`, error);
				this.onError?.(error);
			};

			// Create a new client instance with listChanged handler for tools
			this.client = new Client(
				{
					name: 'llama.cpp-webui',
					version: '1.0.0'
				},
				{
					capabilities: {
						roots: { listChanged: true },
						sampling: {}
					},
					listChanged: {
						tools: {
							onChanged: async (error, tools) => {
								if (error) {
									console.error(`[MCP] Failed to refresh tools for ${this.serverName}:`, error);
									return;
								}
								if (tools) {
									this.onToolsChanged?.(tools);
								}
							}
						}
					}
				}
			);

			// Connect using the transport
			await this.client.connect(this.transport);
			this.reconnectAttempts = 0;

			// Call onOpen callback
			try {
				await this.onOpen?.();
			} catch (err) {
				console.error(`[MCP] Error in onOpen callback for ${this.serverName}:`, err);
				this.onError?.(err instanceof Error ? err : new Error(String(err)));
			}
		} catch (error) {
			console.error(`[MCP] Failed to connect to ${this.serverName}:`, error);
			throw error;
		}
	}

	/**
	 * Build proxy URL for MCP server connection
	 * Uses the C++ backend proxy at /mcp?server={serverName}
	 */
	private _buildProxyUrl(serverName: string): string {
		const url = new URL(window.location.href);
		// Use the same origin with proxy endpoint
		return `${url.origin}/mcp?server=${encodeURIComponent(serverName)}`;
	}

	/**
	 * Disconnect from the MCP server
	 */
	async disconnect() {
		// Set flag to prevent auto-reconnect when onclose fires
		this.manualDisconnect = true;

		// Clear reconnect timer
		if (this.reconnectTimer) {
			clearTimeout(this.reconnectTimer);
			this.reconnectTimer = null;
		}

		// Close the client and transport
		try {
			if (this.client) {
				await this.client.close();
			}
		} catch {
			// Ignore close errors
		}

		try {
			if (this.transport) {
				await this.transport.close();
			}
		} catch {
			// Ignore close errors
		}

		this.client = null;
		this.transport = null;
	}

	/**
	 * Call a tool on the MCP server
	 */
	async callTool(name: string, args?: Record<string, unknown>): Promise<unknown> {
		if (!this.client) {
			throw new Error(`MCP client not initialized for: ${this.serverName}`);
		}

		const startTime = Date.now();

		try {
			const response = await this.client.callTool(
				{
					name,
					arguments: args
				},
				undefined,
				{ timeout: REQUEST_TIMEOUT_MS }
			);

			const duration = Date.now() - startTime;
			console.log(`[MCP] ${this.serverName}/${name} completed in ${duration}ms`);

			if (response.isError) {
				console.error(`[MCP] Tool error ${this.serverName}/${name}:`, response.content);
				throw new Error(`Tool call failed: ${JSON.stringify(response.content)}`);
			}

			return response.content;
		} catch (error) {
			const duration = Date.now() - startTime;
			console.error(`[MCP] Tool failed ${this.serverName}/${name} after ${duration}ms:`, error);
			throw error;
		}
	}

	/**
	 * List available tools from the MCP server
	 */
	async listTools(): Promise<Tool[]> {
		if (!this.client) {
			throw new Error(`MCP client not initialized for: ${this.serverName}`);
		}

		const response = await this.client.listTools(undefined, {
			timeout: REQUEST_TIMEOUT_MS
		});

		return response.tools;
	}

	/**
	 * Initialize the MCP session (called by the SDK automatically during connect)
	 *
	 * Note: The SDK Client handles initialization automatically when connect() is called.
	 * This method is kept for backward compatibility but the actual initialization
	 * is handled by the SDK.
	 */
	async initialize(_capabilities: Record<string, unknown> = {}): Promise<void> {
		console.log(`[MCP] initialize() called for ${this.serverName}`);
		// SDK handles initialization automatically during connect()
		console.log(`[MCP] initialize() completed for ${this.serverName} (handled by SDK)`);
	}

	/**
	 * Send a ping to the MCP server
	 */
	async ping(): Promise<void> {
		if (!this.client) {
			throw new Error(`MCP client not initialized for: ${this.serverName}`);
		}

		await this.client.ping({ timeout: REQUEST_TIMEOUT_MS });
	}

	/**
	 * Check if the connection is currently open
	 */
	isConnected(): boolean {
		// Check if transport's WebSocket is connected
		return this.transport !== null;
	}

	/**
	 * Schedule reconnection attempt
	 */
	private scheduleReconnect() {
		if (this.reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
			console.error(`[MCP] Max reconnect attempts reached for: ${this.serverName}`);
			this.onError?.(new Error('Max reconnect attempts reached'));
			return;
		}

		const delay = Math.min(
			INITIAL_RECONNECT_DELAY_MS * Math.pow(2, this.reconnectAttempts),
			MAX_RECONNECT_DELAY_MS
		);
		this.reconnectAttempts++;

		console.log(
			`[MCP] Scheduling reconnect attempt ${this.reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS} in ${delay}ms`
		);

		this.reconnectTimer = setTimeout(() => {
			console.log(`[MCP] Attempting to reconnect to: ${this.serverName}`);
			this.connect().catch((err) => this.onError?.(err));
		}, delay);
	}
}

/**
 * Factory for creating McpService instances
 */
export const mcpServiceFactory = {
	/**
	 * Create an MCP service for a given server name.
	 *
	 * @param serverName - Name of the MCP server
	 * @returns Configured McpService instance
	 *
	 * @example
	 * mcpServiceFactory.create('brave-search')
	 */
	create: (serverName: string): McpService => {
		return new McpService(serverName);
	}
};
