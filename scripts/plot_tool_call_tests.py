#!/usr/bin/env python3
"""
Model Performance Analysis and Visualization Tool

This script analyzes JSON performance data for different model implementations and tests,
creating a heatmap visualization of success ratios. It handles multiple input files and
supports various model configurations.

Usage:
    python script.py input_file1.json [input_file2.json ...]
"""

import json
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import sys
from typing import Dict, List, Tuple, Set, Any
from pathlib import Path
import logging

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

class ModelAnalyzer:
    def __init__(self):
        self.lines: List[Dict] = []
        self.data_dict: Dict[Tuple, float] = {}
        self.models: List[str] = []
        self.temps: Set[float] = set()
        self.tests: Set[str] = set()
        self.impls: Set[str] = set()
        
        self.column_groups = [
            ("llama-server", []),  # Tests will be populated dynamically
            ("llama-server (no grammar)", []),
            ("ollama", [])
        ]

    def read_files(self, files: List[str]) -> None:
        """Read and parse JSON data from input files."""
        for file in files:
            path = Path(file)
            if not path.exists():
                logger.error(f"File not found: {file}")
                continue
                
            try:
                with path.open() as f:
                    raw_data = f.read()
                logger.info(f"Reading {file} ({len(raw_data)} bytes)")
                
                for line_num, line in enumerate(raw_data.split('\n'), 1):
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        record = json.loads(line)
                        self.lines.append(record)
                    except json.JSONDecodeError as e:
                        logger.warning(f"Invalid JSON at {file}:{line_num} - {e}")
            except Exception as e:
                logger.error(f"Error processing {file}: {e}")

    def process_data(self) -> None:
        """Process the loaded data and organize it for visualization."""
        for rec in self.lines:
            try:
                model = rec["model"]
                temp = rec["temp"]
                impl = rec["implementation"]
                test = rec["test"]
                success = rec["success_ratio"]
                
                self.data_dict[(model, temp, impl, test)] = success
                
                if model not in self.models:
                    self.models.append(model)
                self.temps.add(temp)
                self.tests.add(test)
                self.impls.add(impl)
                
            except KeyError as e:
                logger.warning(f"Missing required field in record: {e}")

        # Sort the collected values
        self.temps = sorted(list(self.temps), key=lambda x: x if x is not None else -1)
        self.tests = sorted(list(self.tests))
        
        # Update column groups with actual tests
        self.column_groups = [
            (impl, list(self.tests)) for impl, _ in self.column_groups
            if impl in self.impls
        ]

    def create_matrix(self) -> pd.DataFrame:
        """Create a matrix for visualization."""
        all_cols = [
            (impl, test)
            for impl, tests in self.column_groups
            for test in tests
        ]
        
        matrix = []
        index = []
        
        for model in self.models:
            for temp in self.temps:
                index.append(f"{model} @ {temp}")
                row_vals = [
                    self.data_dict.get((model, temp, impl, test), np.nan)
                    for impl, test in all_cols
                ]
                matrix.append(row_vals)
        
        # Create column labels
        col_labels = [f"{impl}\n({test})" for impl, test in all_cols]
        
        return pd.DataFrame(matrix, index=index, columns=col_labels)

    def plot_heatmap(self, df: pd.DataFrame, output_file: str = None) -> None:
        """Create and display/save the heatmap visualization."""
        plt.figure(figsize=(12, 6))
        
        sns.heatmap(
            df,
            annot=True,
            cmap="RdYlGn",
            vmin=0.0,
            vmax=1.0,
            cbar=True,
            fmt=".2f",
            center=0.5,
            square=True,
            linewidths=0.5,
            cbar_kws={"label": "Success Ratio"}
        )
        
        plt.title("Model Performance Analysis\nSuccess Ratios by Implementation & Test", 
                 pad=20)
        plt.xlabel("Implementation and Test", labelpad=10)
        plt.ylabel("Model @ Temperature", labelpad=10)
        
        plt.xticks(rotation=45, ha='right')
        plt.yticks(rotation=0)
        
        plt.tight_layout()
        
        if output_file:
            plt.savefig(output_file, dpi=300, bbox_inches='tight')
            logger.info(f"Plot saved to {output_file}")
        else:
            plt.show()

def main():
    if len(sys.argv) < 2:
        logger.error("Please provide at least one input file")
        sys.exit(1)
    
    analyzer = ModelAnalyzer()
    
    # Process input files
    analyzer.read_files(sys.argv[1:])
    
    if not analyzer.lines:
        logger.error("No valid data was loaded")
        sys.exit(1)
    
    # Process the data
    analyzer.process_data()
    
    # Log summary statistics
    logger.info(f"Processed {len(analyzer.lines)} lines")
    logger.info(f"Found {len(analyzer.data_dict)} valid data points")
    logger.info(f"Models: {analyzer.models}")
    logger.info(f"Temperatures: {analyzer.temps}")
    logger.info(f"Tests: {analyzer.tests}")
    logger.info(f"Implementations: {analyzer.impls}")
    
    # Create and plot the visualization
    df = analyzer.create_matrix()
    # analyzer.plot_heatmap(df, "model_analysis.png")
    analyzer.plot_heatmap(df)

if __name__ == "__main__":
    main()