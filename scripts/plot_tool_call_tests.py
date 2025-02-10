import json
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import sys
import re

file = sys.argv[1]
with open(file) as f:
  raw_data = f.read()
lines = []
for line in raw_data.split('\n'):
    line = line.strip()
    if not line:
        continue
    # Try to parse as JSON:
    try:
        record = json.loads(line)
        lines.append(record)
    except json.JSONDecodeError:
        # Skip lines that aren’t valid JSON objects
        pass

# Now store success_ratio in a dict keyed by (temp, implementation, test).
data_dict = {}
for rec in lines:
    temp = rec.get("temp")
    impl = rec.get("implementation")
    test = rec.get("test")
    success = rec.get("success_ratio")
    if temp is not None and impl is not None and test is not None and success is not None:
        data_dict[(temp, impl, test)] = success

temps = set()
tests = set()
impls = set()
for (temp, impl, test) in data_dict.keys():
    temps.add(temp)
    tests.add(test)
    impls.add(impl)
temps = sorted(list(temps))
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

# Create a 2D list (rows x columns) for the final heatmap
matrix = []
for temp in temps:
    row_vals = []
    for (impl, test) in all_cols:
        # Get success ratio if present, else 0 (or NaN if you prefer)
        val = data_dict.get((temp, impl, test), np.nan)
        row_vals.append(val)
    matrix.append(row_vals)

# Convert to a Pandas DataFrame for easier plotting
# We can label columns as "impl: test" or keep them as a MultiIndex
col_labels = [f"{impl}\n({test})" for (impl, test) in all_cols]
df = pd.DataFrame(matrix, index=temps, columns=col_labels)

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
plt.ylabel("Temperature")
plt.tight_layout()
plt.show()
