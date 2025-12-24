<script lang="ts">
	import {
		ChevronDown,
		ChevronRight,
		FileJson,
		FileText,
		Image as ImageIcon,
		Music,
		Link
	} from '@lucide/svelte';
	import { cn } from '$lib/components/ui/utils';
	import { copyToClipboard } from '$lib/utils';
	import {
		formatMcpToolName,
		parseMcpToolResult,
		getContentTypeLabel,
		type McpContentItem
	} from '$lib/utils/tool-results';

	interface Props {
		toolName: string;
		result: string;
		class?: string;
	}

	let { toolName, result, class: className = '' }: Props = $props();

	let isExpanded = $state(false);
	let imageExpanded = $state(false);

	// Parse MCP tool names: mcp__serverName__toolName -> serverName:toolName
	let displayToolName = $derived(formatMcpToolName(toolName));

	// Parse the result using MCP SDK types
	let parsedResult = $derived.by(() => {
		try {
			const parsed = JSON.parse(result);
			return parseMcpToolResult(parsed);
		} catch {
			// Not JSON, treat as plain text
			return {
				content: [{ type: 'text', text: result } as const],
				isError: false
			};
		}
	});

	// Check if result has structured content (more than just plain text)
	let hasStructuredContent = $derived.by(
		() =>
			parsedResult.content.length > 1 ||
			(parsedResult.content.length === 1 && parsedResult.content[0].type !== 'text')
	);

	// Get content type badge for structured results
	let contentTypeBadge = $derived.by(() => {
		const content = parsedResult.content;
		if (content.length === 0) return '';
		if (content.length === 1) return getContentTypeLabel(content[0]);
		return `${content.length} items`;
	});

	function handleCopy() {
		void copyToClipboard(result, 'Tool result copied to clipboard');
	}

	// Get icon component for a content item
	function getIconComponent(item: McpContentItem) {
		switch (item.type) {
			case 'text':
				return FileText;
			case 'image':
				return ImageIcon;
			case 'audio':
				return Music;
			case 'resource':
			case 'resource_link':
				return Link;
		}
	}
</script>

<div class={cn('w-full max-w-[80%]', className)}>
	<div class="overflow-hidden rounded-lg border bg-muted/50">
		<!-- Header - always visible -->
		<div class="flex items-center justify-between px-3 py-2">
			<button
				type="button"
				onclick={() => (isExpanded = !isExpanded)}
				class="flex items-center gap-2 rounded px-2 py-1 text-left transition hover:bg-muted-foreground/5"
			>
				<FileJson class="h-4 w-4 text-muted-foreground" />
				<span class="text-sm font-medium">Tool Result: {displayToolName}</span>
				{#if hasStructuredContent && contentTypeBadge}
					<span class="text-xs text-muted-foreground">({contentTypeBadge})</span>
				{/if}
				{#if parsedResult.isError}
					<span class="text-xs text-destructive">(error)</span>
				{/if}
				{#if isExpanded}
					<ChevronDown class="ml-1 h-4 w-4 text-muted-foreground" />
				{:else}
					<ChevronRight class="ml-1 h-4 w-4 text-muted-foreground" />
				{/if}
			</button>
			<button
				type="button"
				onclick={handleCopy}
				class="rounded p-1.5 transition hover:bg-muted-foreground/10"
				title="Copy result"
				aria-label="Copy result"
			>
				<svg
					xmlns="http://www.w3.org/2000/svg"
					width="14"
					height="14"
					viewBox="0 0 24 24"
					fill="none"
					stroke="currentColor"
					stroke-width="2"
					stroke-linecap="round"
					stroke-linejoin="round"
					class="text-muted-foreground"
					><rect width="14" height="14" x="8" y="8" rx="2" ry="2" /><path
						d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2"
					/></svg
				>
			</button>
		</div>

		<!-- Content - collapsed by default -->
		{#if isExpanded}
			<div class="border-t bg-background/50">
				<div class="max-h-96 overflow-y-auto p-3">
					{#each parsedResult.content as item (item.type + (item.type === 'text' ? item.text : item.type === 'image' ? item.data : item.type === 'audio' ? item.data : item.type === 'resource' ? item.resource.uri : item.type === 'resource_link' ? item.uri : ''))}
						{@const Icon = getIconComponent(item)}
						<div class="mb-3 last:mb-0">
							<!-- Content item header -->
							<div class="mb-1 flex items-center gap-1 text-xs text-muted-foreground">
								<Icon class="h-3 w-3" />
								<span>{getContentTypeLabel(item)}</span>
							</div>

							<!-- Text content -->
							{#if item.type === 'text'}
								<pre class="text-xs break-words whitespace-pre-wrap">{item.text}</pre>

								<!-- Image content -->
							{:else if item.type === 'image'}
								<div>
									<button
										type="button"
										onclick={() => (imageExpanded = !imageExpanded)}
										class="block"
									>
										<img
											src="data:{item.mimeType || 'image/png'};base64,{item.data}"
											alt=""
											class={cn(
												'rounded border transition',
												imageExpanded ? 'max-w-full' : 'max-w-[200px] hover:opacity-80'
											)}
										/>
									</button>
									<button
										type="button"
										onclick={() => (imageExpanded = !imageExpanded)}
										class="mt-1 text-xs text-muted-foreground hover:text-foreground"
									>
										{imageExpanded ? 'Click to shrink' : 'Click to expand'}
									</button>
								</div>

								<!-- Audio content -->
							{:else if item.type === 'audio'}
								<audio controls class="w-full">
									<source src="data:{item.mimeType || 'audio/mp3'};base64,{item.data}" />
									Your browser does not support audio.
								</audio>

								<!-- Resource content -->
							{:else if item.type === 'resource'}
								<a
									href={item.resource.uri}
									target="_blank"
									rel="noopener noreferrer"
									class="flex items-center gap-1 text-sm text-blue-500 underline hover:text-blue-400"
								>
									{item.resource.uri}
								</a>
								<button
									type="button"
									onclick={() => void copyToClipboard(item.resource.uri, 'URI copied to clipboard')}
									class="ml-2 rounded px-1 py-0.5 text-xs text-muted-foreground hover:bg-muted-foreground/10"
								>
									Copy
								</button>

								<!-- Resource link content -->
							{:else if item.type === 'resource_link'}
								<a
									href={item.uri}
									target="_blank"
									rel="noopener noreferrer"
									class="flex items-center gap-1 text-sm text-blue-500 underline hover:text-blue-400"
								>
									{item.uri}
								</a>
								<button
									type="button"
									onclick={() => void copyToClipboard(item.uri, 'URI copied to clipboard')}
									class="ml-2 rounded px-1 py-0.5 text-xs text-muted-foreground hover:bg-muted-foreground/10"
								>
									Copy
								</button>
							{/if}
						</div>
					{/each}
				</div>
			</div>
		{/if}
	</div>
</div>
