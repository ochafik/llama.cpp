These templates can be updated with the following commands:

```bash
./scripts/get_chat_template.py --output-dir models/templates swiss-ai/Apertus-8B-Instruct-2509
./scripts/get_chat_template.py --output-dir models/templates ByteDance/Seed-OSS
./scripts/get_chat_template.py --output-dir models/templates CohereForAI/c4ai-command-r-plus tool_use
./scripts/get_chat_template.py --output-dir models/templates CohereForAI/c4ai-command-r7b-12-2024 default
./scripts/get_chat_template.py --output-dir models/templates CohereForAI/c4ai-command-r7b-12-2024 rag
./scripts/get_chat_template.py --output-dir models/templates CohereForAI/c4ai-command-r7b-12-2024 tool_use
./scripts/get_chat_template.py --output-dir models/templates deepseek-ai/DeepSeek-R1-Distill-Llama-8B
./scripts/get_chat_template.py --output-dir models/templates deepseek-ai/DeepSeek-R1-Distill-Qwen-32B
./scripts/get_chat_template.py --output-dir models/templates deepseek-ai/DeepSeek-V3.1
./scripts/get_chat_template.py --output-dir models/templates fireworks-ai/llama-3-firefunction-v2
./scripts/get_chat_template.py --output-dir models/templates GLM/4.6
./scripts/get_chat_template.py --output-dir models/templates google/gemma-2-2b-it
./scripts/get_chat_template.py --output-dir models/templates Kimi/K2-Thinking
./scripts/get_chat_template.py --output-dir models/templates meetkai/functionary-medium-v3.1
./scripts/get_chat_template.py --output-dir models/templates meetkai/functionary-medium-v3.2
./scripts/get_chat_template.py --output-dir models/templates meta-llama/Llama-3.1-8B-Instruct
./scripts/get_chat_template.py --output-dir models/templates meta-llama/Llama-3.2-3B-Instruct
./scripts/get_chat_template.py --output-dir models/templates meta-llama/Llama-3.3-70B-Instruct
./scripts/get_chat_template.py --output-dir models/templates microsoft/Phi-3.5-mini-instruct
./scripts/get_chat_template.py --output-dir models/templates MiMo/VL
./scripts/get_chat_template.py --output-dir models/templates MiniMaxAI/MiniMax-M2
./scripts/get_chat_template.py --output-dir models/templates mistralai/Ministral-3-14B-Reasoning-2512
./scripts/get_chat_template.py --output-dir models/templates mistralai/Mistral-Nemo-Instruct-2407
./scripts/get_chat_template.py --output-dir models/templates NousResearch/Hermes-2-Pro-Llama-3-8B tool_use
./scripts/get_chat_template.py --output-dir models/templates NousResearch/Hermes-3-Llama-3.1-8B tool_use
./scripts/get_chat_template.py --output-dir models/templates NVIDIA/Nemotron-3-Nano-30B-A3B-BF16
./scripts/get_chat_template.py --output-dir models/templates NVIDIA/Nemotron-Nano-v2
./scripts/get_chat_template.py --output-dir models/templates openai/gpt-oss-120b
./scripts/get_chat_template.py --output-dir models/templates path/path/Qwen/Qwen2.5-7B-Instruct
./scripts/get_chat_template.py --output-dir models/templates ServiceNow-AI/Apriel-1.5-15b-Thinker
./scripts/get_chat_template.py --output-dir models/templates Qwen/Qwen2.5-7B-Instruct
./scripts/get_chat_template.py --output-dir models/templates Qwen/Qwen3-0.6B
./scripts/get_chat_template.py --output-dir models/templates Qwen/QwQ-32B
./scripts/get_chat_template.py --output-dir models/templates Qwen3/Coder
./scripts/get_chat_template.py --output-dir models/templates unsloth/Apriel-1.5
./scripts/get_chat_template.py --output-dir models/templates unsloth/Magistral-Small-2509
./scripts/get_chat_template.py --output-dir models/templates zai-org/GLM-4.5
```

Each command saves both the `.jinja` template and a `.metadata.json` file with the model's metadata (including EOS tokens).
