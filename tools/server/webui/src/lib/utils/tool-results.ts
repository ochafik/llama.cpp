/**
 * Utility functions for handling MCP tool result messages
 */

import type {
	TextContent,
	ImageContent,
	AudioContent,
	EmbeddedResource,
	ResourceLink,
	CallToolResult
} from '@modelcontextprotocol/sdk/types.js';

export interface ToolResultInfo {
	toolName: string;
	result: string;
}

export type McpContentItem =
	| TextContent
	| ImageContent
	| AudioContent
	| EmbeddedResource
	| ResourceLink;

/**
 * Check if a message content is a tool result message
 */
export function isToolResultContent(content: string): boolean {
	return content.trim().startsWith('[Tool Result: ');
}

/**
 * Check if a database message is a tool result message
 */
export function isToolResultMessage(msg: { role: string; content: string }): boolean {
	return msg.role === 'user' && isToolResultContent(msg.content);
}

/**
 * Parse tool result from message content
 * Handles both \n and \r\n line endings, and empty results
 */
export function parseToolResult(content: string): ToolResultInfo | null {
	const trimmed = content.trim();

	// Handle both \n and \r\n line endings
	const match = trimmed.match(/^\[Tool Result: ([^\]]+)\][\r\n]+([\s\S]*)$/);
	if (match) {
		return { toolName: match[1], result: match[2] };
	}

	// Also try without newline (in case result is empty)
	const matchNoNewline = trimmed.match(/^\[Tool Result: ([^\]]+)\]$/);
	if (matchNoNewline) {
		return { toolName: matchNoNewline[1], result: '' };
	}

	return null;
}

/**
 * Format an MCP qualified tool name for display
 * Converts "mcp__serverName__toolName" to "serverName:toolName"
 */
export function formatMcpToolName(qualifiedName: string): string {
	if (!qualifiedName.startsWith('mcp__')) {
		return qualifiedName;
	}
	const parts = qualifiedName.split('__');
	if (parts.length < 3) {
		return qualifiedName;
	}
	// Handle tool names that may contain __ themselves
	return `${parts[1]}:${parts.slice(2).join('__')}`;
}

/**
 * Create a tool result message content string
 */
export function createToolResultContent(toolName: string, result: string): string {
	return `[Tool Result: ${toolName}]\n${result}`;
}

/**
 * Parse MCP tool result using SDK types
 * Tries 'content' first, then 'structuredContent', then fallback to string/JSON
 */
export function parseMcpToolResult(result: unknown): CallToolResult {
	// Try MCP spec format with 'content' array
	if (isCallToolResult(result)) {
		return result;
	}

	// Fallback to structuredContent field (some implementations use this)
	if (result && typeof result === 'object' && 'structuredContent' in result) {
		const items = (result as { structuredContent: unknown[] }).structuredContent || [];
		return {
			content: items as CallToolResult['content'],
			isError: false
		};
	}

	// Fallback: treat as raw text/stringified JSON
	let rawText = '';
	if (result === null || result === undefined) {
		rawText = '';
	} else if (typeof result === 'string') {
		rawText = result;
	} else {
		rawText = JSON.stringify(result, null, 2);
	}

	return {
		content: [{ type: 'text', text: rawText } as const],
		isError: false
	};
}

/**
 * Type guard for CallToolResult
 */
function isCallToolResult(value: unknown): value is CallToolResult {
	if (!value || typeof value !== 'object') {
		return false;
	}

	const v = value as Record<string, unknown>;

	// Check for content array (MCP spec format)
	if ('content' in v) {
		const content = v.content;
		return Array.isArray(content) && content.every(isContentItem);
	}

	return false;
}

/**
 * Type guard for ContentItem
 */
function isContentItem(value: unknown): value is McpContentItem {
	if (!value || typeof value !== 'object') {
		return false;
	}

	const v = value as Record<string, unknown>;

	// Must have a type field
	if (typeof v.type !== 'string') {
		return false;
	}

	// Validate based on type
	switch (v.type) {
		case 'text':
			return typeof v.text === 'string';
		case 'image':
			return typeof v.data === 'string';
		case 'audio':
			return typeof v.data === 'string';
		case 'resource':
			return typeof v.uri === 'string';
		default:
			return false;
	}
}

/**
 * Get a display label for a content item type
 */
export function getContentTypeLabel(item: McpContentItem): string {
	switch (item.type) {
		case 'text':
			return 'Text';
		case 'image':
			return `Image${item.mimeType ? ` (${item.mimeType})` : ''}`;
		case 'audio':
			return `Audio${item.mimeType ? ` (${item.mimeType})` : ''}`;
		case 'resource':
			return `Resource: ${item.resource.uri}`;
		case 'resource_link':
			return `Resource Link: ${item.uri}`;
		default:
			return 'Unknown';
	}
}
