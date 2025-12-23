<script lang="ts">
	import { onMount, tick } from 'svelte';
	import {
		ChevronDown,
		ChevronRight,
		Circle,
		CircleDashed,
		Loader2,
		Plug,
		RefreshCw,
		X,
		FileJson,
		Wrench
	} from '@lucide/svelte';
	import * as Popover from '$lib/components/ui/popover';
	import { cn } from '$lib/components/ui/utils';
	import { mcpStore } from '$lib/stores/mcp.svelte';
	import { SearchInput } from '$lib/components/app';
	import { SvelteSet, SvelteMap } from 'svelte/reactivity';

	interface Props {
		class?: string;
		disabled?: boolean;
	}

	let { class: className = '', disabled = false }: Props = $props();

	// UI State
	let isOpen = $state(false);
	let searchTerm = $state('');
	let searchInputRef = $state<HTMLInputElement | null>(null);
	let selectedServerName = $state<string | null>(null);
	let highlightedIndex = $state<number>(-1);

	// Server data
	let availableServers = $state<string[]>([]);
	let loadingServers = $state(true);
	let fetchingTools = new SvelteSet<string>();

	// Filtered servers for search
	let filteredServers = $derived(
		(() => {
			const term = searchTerm.trim().toLowerCase();
			if (!term) return availableServers;
			return availableServers.filter((s) => s.toLowerCase().includes(term));
		})()
	);

	// Reactive server status map - tracks connection states reactively
	// This MUST be a $derived.by to properly track changes to mcpStore
	let serverStatus = $derived.by(() => {
		const statusMap = new SvelteMap<
			string,
			{
				connected: boolean;
				connecting: boolean;
				error?: string;
			}
		>();
		for (const serverName of availableServers) {
			const connection = mcpStore.connections.get(serverName);
			statusMap.set(serverName, {
				connected: connection?.connected ?? false,
				connecting: mcpStore.isConnecting(serverName),
				error: connection?.error
			});
		}
		return statusMap;
	});

	// Count connected servers - MUST be computed from serverStatus to be reactive
	let connectedCount = $derived.by(() => {
		return Array.from(serverStatus.values()).filter((s) => s.connected).length;
	});

	// Get server status (now uses the reactive map)
	function getServerStatus(serverName: string): {
		connected: boolean;
		connecting: boolean;
		error?: string;
	} {
		return serverStatus.get(serverName) ?? { connected: false, connecting: false };
	}

	// Get status color and icon for server
	function getStatusDisplay(status: { connected: boolean; connecting: boolean; error?: string }) {
		if (status.connecting) {
			return {
				color: 'text-orange-500',
				bgColor: 'bg-orange-500',
				icon: Loader2,
				label: 'Connecting...'
			};
		}
		if (status.connected) {
			return { color: 'text-green-500', bgColor: 'bg-green-500', icon: Circle, label: 'Connected' };
		}
		if (status.error) {
			return { color: 'text-red-500', bgColor: 'bg-red-500', icon: Circle, label: 'Error' };
		}
		return {
			color: 'text-muted-foreground',
			bgColor: 'bg-muted-foreground',
			icon: CircleDashed,
			label: 'Disconnected'
		};
	}

	// Fetch available servers
	async function fetchAvailableServers() {
		loadingServers = true;
		try {
			const response = await fetch('/mcp/servers');
			if (!response.ok) throw new Error('Failed to fetch MCP servers');
			const data = await response.json();
			availableServers = data.servers.map((s: { name: string }) => s.name);
		} catch (error) {
			console.error('Failed to fetch MCP servers:', error);
			availableServers = [];
		} finally {
			loadingServers = false;
		}
	}

	onMount(() => {
		fetchAvailableServers();
	});

	// Handle popover open/close
	async function handleOpenChange(open: boolean) {
		if (loadingServers) return;

		isOpen = open;
		if (open) {
			searchTerm = '';
			selectedServerName = null;
			await fetchAvailableServers();
			tick().then(() => {
				requestAnimationFrame(() => searchInputRef?.focus());
			});
		} else {
			selectedServerName = null;
		}
	}

	// Toggle connection
	async function toggleConnection(serverName: string) {
		const status = getServerStatus(serverName);
		if (status.connecting) return;

		if (status.connected) {
			mcpStore.disconnect(serverName);
		} else {
			await mcpStore.connect(serverName);
		}
	}

	// Connect to all servers
	async function connectAll() {
		for (const serverName of availableServers) {
			const status = getServerStatus(serverName);
			if (!status.connected && !status.connecting) {
				mcpStore.connect(serverName).catch((err) => {
					console.error(`[MCP] Failed to connect to ${serverName}:`, err);
				});
			}
		}
	}

	// Disconnect all servers
	function disconnectAll() {
		for (const serverName of availableServers) {
			mcpStore.disconnect(serverName);
		}
	}

	// Check if any servers are connected or connecting
	let hasAnyConnection = $derived(() => {
		return availableServers.some((s) => {
			const status = getServerStatus(s);
			return status.connected || status.connecting;
		});
	});

	// Check if all servers are connected
	let allConnected = $derived(() => {
		return (
			availableServers.length > 0 && availableServers.every((s) => getServerStatus(s).connected)
		);
	});

	// Force reconnect
	async function forceReconnect(serverName: string) {
		try {
			mcpStore.disconnect(serverName);
			// Wait for WebSocket to fully close before reconnecting
			// mcpStore.connect() has an internal 500ms wait, so we add buffer
			await new Promise((r) => setTimeout(r, 600));
			await mcpStore.connect(serverName);
		} catch (error) {
			console.error(`[MCP] Reconnect failed for ${serverName}:`, error);
		}
	}

	// Get tools for a server
	function getTools(serverName: string) {
		return mcpStore.getTools(serverName);
	}

	// Fetch tools for a server
	async function fetchServerTools(serverName: string) {
		if (!mcpStore.isConnected(serverName)) return;
		fetchingTools.add(serverName);
		try {
			// Tools are automatically fetched on connect, but we can refresh
			const service = mcpStore.connections.get(serverName);
			if (service?.connected) {
				// Tools should already be loaded from the initial connection
			}
		} finally {
			fetchingTools.delete(serverName);
		}
	}

	// Show server details
	function showServerDetails(serverName: string) {
		selectedServerName = serverName;
		if (mcpStore.isConnected(serverName)) {
			fetchServerTools(serverName);
		}
	}

	// Go back to server list
	function backToServerList() {
		selectedServerName = null;
	}

	// Handle keyboard navigation in search
	function handleSearchKeyDown(event: KeyboardEvent) {
		if (event.isComposing) return;

		if (event.key === 'ArrowDown') {
			event.preventDefault();
			if (filteredServers.length === 0) return;
			highlightedIndex = highlightedIndex < filteredServers.length - 1 ? highlightedIndex + 1 : 0;
		} else if (event.key === 'ArrowUp') {
			event.preventDefault();
			if (filteredServers.length === 0) return;
			highlightedIndex = highlightedIndex > 0 ? highlightedIndex - 1 : filteredServers.length - 1;
		} else if (event.key === 'Enter' && highlightedIndex >= 0) {
			event.preventDefault();
			showServerDetails(filteredServers[highlightedIndex]);
		}
	}
</script>

<div class={cn('relative inline-flex flex-col items-end gap-1', className)}>
	<Popover.Root bind:open={isOpen} onOpenChange={handleOpenChange}>
		<Popover.Trigger
			class={cn(
				`inline-flex cursor-pointer items-center gap-1.5 rounded-sm bg-muted-foreground/10 px-1.5 py-1 text-xs transition hover:text-foreground focus:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 disabled:cursor-not-allowed disabled:opacity-60`,
				isOpen ? 'text-foreground' : 'text-muted-foreground'
			)}
			{disabled}
		>
			<Plug class="h-3.5 w-3.5" />
			<span class="truncate font-medium">
				{availableServers.length > 0 ? `${connectedCount}/${availableServers.length} MCP` : 'MCP'}
			</span>
			<ChevronDown class="h-3 w-3.5" />
		</Popover.Trigger>

		<Popover.Content
			class="w-96 max-w-[calc(100vw-2rem)] p-0"
			align="end"
			sideOffset={8}
			collisionPadding={16}
		>
			<div class="flex max-h-[50dvh] flex-col overflow-hidden">
				{#if selectedServerName}
					<!-- Server Details View -->
					{@const status = getServerStatus(selectedServerName)}
					{@const statusDisplay = getStatusDisplay(status)}
					{@const tools = getTools(selectedServerName)}
					{@const isFetching = fetchingTools.has(selectedServerName)}

					<div class="flex items-center gap-2 border-b p-4">
						<button
							type="button"
							onclick={backToServerList}
							class="shrink-0 rounded-sm p-1 transition hover:bg-muted"
						>
							<ChevronDown class="h-4 w-4 rotate-90" />
						</button>
						<div class="flex min-w-0 flex-1 items-center gap-2">
							<statusDisplay.icon
								class="h-4 w-4 {statusDisplay.color} {status.connecting ? 'animate-spin' : ''}"
							/>
							<span class="truncate font-medium">{selectedServerName}</span>
						</div>
						<button
							onclick={() => selectedServerName && forceReconnect(selectedServerName)}
							class="shrink-0 rounded p-1 transition hover:bg-muted disabled:opacity-50"
							disabled={status.connecting || !selectedServerName}
							title="Reconnect"
						>
							<RefreshCw class="h-4 w-4 {status.connecting ? 'animate-spin' : ''}" />
						</button>
					</div>

					<div class="flex-1 overflow-y-auto p-4">
						{#if isFetching}
							<div class="flex items-center justify-center py-8 text-muted-foreground">
								<Loader2 class="mr-2 h-5 w-5 animate-spin" />
								Loading tools...
							</div>
						{:else if !status.connected}
							<div class="py-8 text-center text-muted-foreground">
								<p class="mb-4">Server is not connected</p>
								<button
									onclick={() => selectedServerName && toggleConnection(selectedServerName)}
									class="inline-flex items-center gap-2 rounded-md bg-primary px-4 py-2 text-sm font-medium text-primary-foreground hover:bg-primary/90"
									disabled={!selectedServerName}
								>
									<Plug class="h-4 w-4" />
									Connect to Server
								</button>
							</div>
						{:else if tools.length === 0}
							<div class="py-8 text-center text-muted-foreground">
								<p>No tools available from this server</p>
							</div>
						{:else}
							<div class="space-y-3">
								<h4 class="text-sm font-medium text-muted-foreground">
									{tools.length} Tool{tools.length !== 1 ? 's' : ''} Available
								</h4>
								{#each tools as tool (tool.name)}
									<div class="group rounded-md border p-3 transition hover:bg-muted/50">
										<div class="flex items-start gap-2">
											<Wrench class="mt-0.5 h-4 w-4 shrink-0 text-muted-foreground" />
											<div class="min-w-0 flex-1">
												<p class="truncate text-sm font-medium">{tool.name}</p>
												{#if tool.description}
													<p class="mt-1 line-clamp-2 text-xs text-muted-foreground">
														{tool.description}
													</p>
												{/if}
												{#if tool.inputSchema}
													<div class="mt-2">
														<button
															type="button"
															class="flex items-center gap-1 text-xs text-muted-foreground transition hover:text-foreground"
															title={JSON.stringify(tool.inputSchema, null, 2)}
														>
															<FileJson class="h-3 w-3" />
															View input schema
														</button>
													</div>
												{/if}
											</div>
										</div>
									</div>
								{/each}
							</div>
						{/if}
					</div>
				{:else}
					<!-- Server List View -->
					<div class="shrink-0 border-b p-4">
						<SearchInput
							id="mcp-search"
							placeholder="Search servers..."
							bind:value={searchTerm}
							bind:ref={searchInputRef}
							onClose={() => handleOpenChange(false)}
							onKeyDown={handleSearchKeyDown}
						/>
					</div>

					<div class="min-h-0 flex-1 overflow-y-auto">
						{#if loadingServers}
							<div class="flex items-center justify-center py-8 text-muted-foreground">
								<Loader2 class="mr-2 h-5 w-5 animate-spin" />
								Loading servers...
							</div>
						{:else if availableServers.length === 0}
							<div class="p-4 text-center text-sm text-muted-foreground">
								<p class="mb-2">No MCP servers configured</p>
								<p class="text-xs">Add servers to ~/.llama.cpp/mcp.json</p>
							</div>
						{:else if filteredServers.length === 0}
							<div class="p-4 text-center text-sm text-muted-foreground">
								No servers found matching "{searchTerm}"
							</div>
						{:else}
							{#each filteredServers as serverName, index (serverName)}
								{@const status = getServerStatus(serverName)}
								{@const statusDisplay = getStatusDisplay(status)}
								{@const isHighlighted = index === highlightedIndex}

								<div
									class={cn(
										'group flex w-full items-center gap-2 px-4 py-2 text-sm transition',
										isHighlighted
											? 'bg-accent text-accent-foreground'
											: 'hover:bg-accent hover:text-accent-foreground'
									)}
									onmouseenter={() => (highlightedIndex = index)}
									role="button"
									tabindex="0"
									onclick={() => showServerDetails(serverName)}
									onkeydown={(e) => {
										if (e.key === 'Enter' || e.key === ' ') {
											e.preventDefault();
											showServerDetails(serverName);
										}
									}}
								>
									<statusDisplay.icon
										class="h-3.5 w-3.5 shrink-0 {statusDisplay.color} {status.connecting
											? 'animate-spin'
											: ''}"
									/>

									<span class="min-w-0 flex-1 truncate">{serverName}</span>

									<button
										type="button"
										onclick={(e) => {
											e.stopPropagation();
											toggleConnection(serverName);
										}}
										class="shrink-0 rounded p-1 transition hover:bg-muted disabled:opacity-50"
										disabled={status.connecting}
										aria-label={status.connected ? 'Disconnect' : 'Connect'}
										title={status.connected ? 'Disconnect' : 'Connect'}
									>
										{#if status.connecting}
											<Loader2 class="h-3.5 w-3.5 animate-spin text-orange-500" />
										{:else if status.connected}
											<X class="h-3.5 w-3.5 text-red-500" />
										{:else}
											<Plug class="h-3.5 w-3.5 text-muted-foreground" />
										{/if}
									</button>

									<ChevronRight
										class="h-3.5 w-3.5 shrink-0 text-muted-foreground opacity-0 transition-opacity group-hover:opacity-100"
									/>
								</div>
							{/each}
						{/if}
					</div>

					{#if availableServers.length > 0}
						<div class="shrink-0 border-t p-3">
							<div class="flex items-center justify-between gap-2">
								<span class="text-xs text-muted-foreground">
									{connectedCount} of {availableServers.length} servers connected
								</span>
								<div class="flex gap-1">
									{#if !allConnected()}
										<button
											type="button"
											onclick={connectAll}
											disabled={availableServers.every((s) => getServerStatus(s).connecting)}
											class="inline-flex items-center gap-1 rounded-md bg-primary px-2 py-1 text-xs font-medium text-primary-foreground hover:bg-primary/90 disabled:cursor-not-allowed disabled:opacity-50"
											title="Connect to all servers"
										>
											<Plug class="h-3 w-3" />
											Connect All
										</button>
									{/if}
									{#if hasAnyConnection()}
										<button
											type="button"
											onclick={disconnectAll}
											class="text-destructive-foreground inline-flex items-center gap-1 rounded-md bg-destructive px-2 py-1 text-xs font-medium hover:bg-destructive/90"
											title="Disconnect from all servers"
										>
											<X class="h-3 w-3" />
											Disconnect All
										</button>
									{/if}
								</div>
							</div>
						</div>
					{/if}
				{/if}
			</div>
		</Popover.Content>
	</Popover.Root>
</div>
