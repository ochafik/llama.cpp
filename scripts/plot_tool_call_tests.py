import json
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import sys

lines = []

for file in sys.argv[1:]:
    
    with open(file) as f:
        raw_data = f.read()
    print(f"Reading {file} with {len(raw_data)} bytes")
    for line in raw_data.split('\n'):
        line = line.strip()
        if not line:
            continue
        # Try to parse as JSON:
        try:
            record = json.loads(line)
            lines.append(record)
        except json.JSONDecodeError as e:
            # Skip lines that aren’t valid JSON objects
            print(f"Skipping line: {line} ({e})")
            pass

# Now store success_ratio in a dict keyed by (model, temp, implementation, test).
data_dict = {}
for rec in lines:
    model = rec.get("model")
    temp = rec.get("temp")
    impl = rec.get("implementation")
    test = rec.get("test")
    success = rec.get("success_ratio")
    if model is not None and impl is not None and test is not None and success is not None:
        data_dict[(model, temp, impl, test)] = success

temps = set()
tests = set()
impls = set()
models = list()
for (model, temp, impl, test) in data_dict.keys():
    if model not in models:
        models.append(model)
    temps.add(temp)
    tests.add(test)
    impls.add(impl)
temps = sorted(list(temps), key=lambda x: x if x is not None else -1)
tests = sorted(list(tests))
column_groups = [
    ("llama-server", tests),
    ("llama-server (no grammar)", tests),
    ("ollama", tests)
]

all_cols = []
for (impl, tests) in column_groups:
    if impl in impls:
        for t in tests:
            all_cols.append((impl, t))

print("Lines read:", len(lines))
print("Data points read:", len(data_dict))
print(f"Models: {models}")
print(f"Temps: {temps}")
print(f"Tests: {tests}")
print(f"Implementations: {impls}")
print(f"Columns: {all_cols}")


# Create a 2D list (rows x columns) for the final heatmap
matrix = []
index = []
for model in models:
    for temp in temps:
        index.append(f"{model} @ {temp}")
        row_vals = []
        for (impl, test) in all_cols:
            # Get success ratio if present, else 0 (or NaN if you prefer)
            val = data_dict.get((model, temp, impl, test), np.nan)
            row_vals.append(val)
        matrix.append(row_vals)

# Convert to a Pandas DataFrame for easier plotting
# We can label columns as "impl: test" or keep them as a MultiIndex
col_labels = [f"{impl}\n({test})" for (impl, test) in all_cols]
df = pd.DataFrame(matrix, index=index, columns=col_labels)

# Plot a heatmap using seaborn: success ratio from 0 (red) to 1 (green)
plt.figure(figsize=(10, 4))
sns.heatmap(
    df, 
    annot=True, 
    cmap="RdYlGn", 
    vmin=0.0, 
    vmax=1.0, 
    cbar=True,
    fmt=".2f"
)
plt.title("Success Ratios by Implementation & Test (Rows = Temp, Columns = Implementation:Test)")
plt.xlabel("Implementation and Test")
plt.ylabel("Model @ Temp")
plt.tight_layout()
plt.show()
