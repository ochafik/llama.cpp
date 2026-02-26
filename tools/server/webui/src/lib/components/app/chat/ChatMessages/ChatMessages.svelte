<script lang="ts">
	import { ChatMessage } from '$lib/components/app';
	import { setChatActionsContext } from '$lib/contexts';
	import { MessageRole } from '$lib/enums';
	import { chatStore } from '$lib/stores/chat.svelte';
	import { conversationsStore, activeConversation } from '$lib/stores/conversations.svelte';
	import { config } from '$lib/stores/settings.svelte';
	import {
		copyToClipboard,
		formatMessageForClipboard,
		getMessageSiblings,
		isToolResultMessage,
		parseToolResult
	} from '$lib/utils';

	interface Props {
		class?: string;
		messages?: DatabaseMessage[];
		onUserAction?: () => void;
	}

	let { class: className, messages = [], onUserAction }: Props = $props();

	let allConversationMessages = $state<DatabaseMessage[]>([]);
	const currentConfig = config();

	setChatActionsContext({
		copy: async (message: DatabaseMessage) => {
			const asPlainText = Boolean(currentConfig.copyTextAttachmentsAsPlainText);
			const clipboardContent = formatMessageForClipboard(
				message.content,
				message.extra,
				asPlainText
			);
			await copyToClipboard(clipboardContent, 'Message copied to clipboard');
		},

		delete: async (message: DatabaseMessage) => {
			await chatStore.deleteMessage(message.id);
			refreshAllMessages();
		},

		navigateToSibling: async (siblingId: string) => {
			await conversationsStore.navigateToSibling(siblingId);
		},

		editWithBranching: async (
			message: DatabaseMessage,
			newContent: string,
			newExtras?: DatabaseMessageExtra[]
		) => {
			onUserAction?.();
			await chatStore.editMessageWithBranching(message.id, newContent, newExtras);
			refreshAllMessages();
		},

		editWithReplacement: async (
			message: DatabaseMessage,
			newContent: string,
			shouldBranch: boolean
		) => {
			onUserAction?.();
			await chatStore.editAssistantMessage(message.id, newContent, shouldBranch);
			refreshAllMessages();
		},

		editUserMessagePreserveResponses: async (
			message: DatabaseMessage,
			newContent: string,
			newExtras?: DatabaseMessageExtra[]
		) => {
			onUserAction?.();
			await chatStore.editUserMessagePreserveResponses(message.id, newContent, newExtras);
			refreshAllMessages();
		},

		regenerateWithBranching: async (message: DatabaseMessage, modelOverride?: string) => {
			onUserAction?.();
			await chatStore.regenerateMessageWithBranching(message.id, modelOverride);
			refreshAllMessages();
		},

		continueAssistantMessage: async (message: DatabaseMessage) => {
			onUserAction?.();
			await chatStore.continueAssistantMessage(message.id);
			refreshAllMessages();
		}
	});

	function refreshAllMessages() {
		const conversation = activeConversation();

		if (conversation) {
			conversationsStore.getConversationMessages(conversation.id).then((messages) => {
				allConversationMessages = messages;
			});
		} else {
			allConversationMessages = [];
		}
	}

	// Single effect that tracks both conversation and message changes
	$effect(() => {
		const conversation = activeConversation();

		if (conversation) {
			refreshAllMessages();
		}
	});

	// Check if a message is a tool result (either role:'tool' or legacy user-role format)
	function isToolResult(msg: DatabaseMessage): boolean {
		return msg.role === 'tool' || isToolResultMessage(msg);
	}

	// Build a map from parent message ID -> tool results for ToolCallBlock display
	let toolResultsMap = $derived.by(() => {
		const map = new Map<string, Map<string, { result: string; timestamp?: number }>>();
		for (const msg of messages) {
			if (isToolResult(msg)) {
				const parsed = parseToolResult(msg.content);
				if (parsed && msg.parent) {
					if (!map.has(msg.parent)) {
						map.set(msg.parent, new Map());
					}
					map.get(msg.parent)!.set(parsed.toolName, {
						result: parsed.result,
						timestamp: msg.timestamp
					});
				}
			}
		}
		return map;
	});

	// Set of message IDs that are tool results (to hide from display)
	let toolResultMessageIds = $derived.by(() => {
		const ids = new Set<string>();
		for (const msg of messages) {
			if (isToolResult(msg)) {
				ids.add(msg.id);
			}
		}
		return ids;
	});

	let displayMessages = $derived.by(() => {
		if (!messages.length) {
			return [];
		}

		const filteredMessages = currentConfig.showSystemMessage
			? messages.filter((msg) => !toolResultMessageIds.has(msg.id))
			: messages.filter(
					(msg) => msg.type !== MessageRole.SYSTEM && !toolResultMessageIds.has(msg.id)
				);

		let lastAssistantIndex = -1;

		for (let i = filteredMessages.length - 1; i >= 0; i--) {
			if (filteredMessages[i].role === MessageRole.ASSISTANT) {
				lastAssistantIndex = i;

				break;
			}
		}

		return filteredMessages.map((message, index) => {
			const siblingInfo = getMessageSiblings(allConversationMessages, message.id);
			const isLastAssistantMessage =
				message.role === MessageRole.ASSISTANT && index === lastAssistantIndex;

			return {
				message,
				isLastAssistantMessage,
				toolResults: toolResultsMap.get(message.id),
				siblingInfo: siblingInfo || {
					message,
					siblingIds: [message.id],
					currentIndex: 0,
					totalSiblings: 1
				}
			};
		});
	});
</script>

<div class="flex h-full flex-col space-y-10 pt-24 {className}" style="height: auto; ">
	{#each displayMessages as { message, isLastAssistantMessage, toolResults, siblingInfo } (message.id)}
		<ChatMessage
			class="mx-auto w-full max-w-[48rem]"
			{message}
			{isLastAssistantMessage}
			{toolResults}
			{siblingInfo}
		/>
	{/each}
</div>
