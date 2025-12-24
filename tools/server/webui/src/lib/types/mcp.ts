// MCP (Model Context Protocol) Types
// Re-export SDK types and define frontend-specific types only

import type { Tool } from '@modelcontextprotocol/sdk/types.js';

// Re-export SDK Tool type for convenience
export type { Tool as McpTool };

// MCP Connection state (frontend-specific)
export interface McpConnectionState {
	name: string;
	connected: boolean;
	tools: Tool[];
	error?: string;
}
