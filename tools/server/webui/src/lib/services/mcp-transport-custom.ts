import type { JSONRPCMessage } from '@modelcontextprotocol/sdk/types.js';
import { JSONRPCMessageSchema } from '@modelcontextprotocol/sdk/types.js';
import { type Transport } from '@modelcontextprotocol/sdk/shared/transport.js';

const SUBPROTOCOL = 'mcp';

// modified from WebSocketClientTransport

export class CustomLlamaCppTransport implements Transport {
	private _evSource?: EventSource;
	private _url: URL;
	private _id: string = 'unknown';

	onclose?: () => void;
	onerror?: (error: Error) => void;
	onmessage?: (message: JSONRPCMessage) => void;

	constructor(url: string) {
		this._url = new URL(url);
	}

	start(): Promise<void> {
		if (this._evSource) {
			throw new Error(
				'CustomLlamaCppTransport already started! If using Client class, note that connect() calls start() automatically.'
			);
		}

		return new Promise((resolve, reject) => {
			console.log('Connecting to SSE:', this._url.toString());
			this._evSource = new EventSource(this._url.toString(), { withCredentials: true });

			this._evSource.onerror = (event) => {
				if (event.eventPhase == EventSource.CLOSED) {
					this.onclose?.();
					console.log('Event Source Closed');
				}

				const error =
					'error' in event
						? (event.error as Error)
						: new Error(`EventSource error: ${JSON.stringify(event)}`);
				reject(error);
				this.onerror?.(error);
			};

			// this._evSource.onopen = () => {
			//     resolve();
			// };

			// this._evSource.onclose = () => {
			//     this.onclose?.();
			// };

			this._evSource.onmessage = (event: MessageEvent) => {
				const raw = event.data.startsWith('data: ') ? event.data.slice(6) : event.data;
				console.log('SSE Message received:', raw);
				const data = JSON.parse(raw);
				if (data.llamacpp_id) {
					this._id = data.llamacpp_id;
					console.log('Connected to SSE with id:', this._id);
					resolve();
					return;
				}
				let message: JSONRPCMessage;
				try {
					message = JSONRPCMessageSchema.parse(data);
				} catch (error) {
					this.onerror?.(error as Error);
					return;
				}

				this.onmessage?.(message);
			};
		});
	}

	async close(): Promise<void> {
		this._evSource?.close();
	}

	send(message: JSONRPCMessage): Promise<void> {
		return new Promise((resolve, reject) => {
			if (!this._evSource) {
				reject(new Error('Not connected'));
				return;
			}

			if (this._id == 'unknown') {
				reject(new Error('Connection ID not set yet'));
				return;
			}

			const url = new URL(this._url.toString());
			url.searchParams.append('llamacpp_id', this._id);
			fetch(url.toString(), {
				method: 'POST',
				headers: {
					'Content-Type': 'application/json',
					'X-Subprotocol': SUBPROTOCOL // redundant, maybe remove later
				},
				body: JSON.stringify(message),
				credentials: 'include' // redundant too, maybe remove later
			})
				.then(() => {
					resolve();
				})
				.catch((error) => {
					reject(error);
				});
		});
	}
}
