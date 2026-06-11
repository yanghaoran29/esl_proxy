#!/usr/bin/env python3
"""
Generate DAG visualization from pto._thread_0.csv
Extracts task dependencies and renders a graph.
"""

import re
import csv
import os
import argparse
import subprocess
import json
from collections import defaultdict


# Task type mapping:0=CUBE, 1=VECTOR, 2=MIX
TASK_TYPE_NAMES = {
    0: 'CUBE',
    1: 'VECTOR',
    2: 'MIX',
}

def parse_csv(filepath):
    """Parse CSV and extract task dependencies and task types."""
    edges = []  # (task_id, predecessor_id)
    task_types = {}  # task_id -> type
    
    with open(filepath, 'r') as f:
        reader = csv.reader(f)
        header = next(reader)  # skip header
        
        for row in reader:
            if len(row) < 2:
                continue
            detail = ','.join(row[2:])  # join detail fields
            
            if 'submit' in detail:
                # Extract task_id and type from enqueue records
                # Format: "enqueue task_id,2, type,1, ctrl_id,0, cnt,0"
                task_match = re.search(r'task_id[,\s]+(\d+)', detail)
                type_match = re.search(r'type[,\s]+(\d+)', detail)
                
                if task_match and type_match:
                    task_id = int(task_match.group(1))
                    task_type = int(type_match.group(1))
                    task_types[task_id] = task_type
            
            elif 'succeed' in detail:
                # Extract task_id and predecessor using regex
                # Format: "succeed task_id,3,predecessor,2,target,2"
                task_match = re.search(r'task_id[,\s]+(\d+)', detail)
                pred_match = re.search(r'predecessor_id[,\s]+(\d+)', detail)
                
                if task_match and pred_match:
                    task_id = int(task_match.group(1))
                    pred_id = int(pred_match.group(1))
                    edges.append((task_id, pred_id))
    
    return edges, task_types


def generate_dot(task_types, edges, output_path):
    """Generate DOT file for DAG visualization.
    
    Args:
        task_types: List/array where index is node id and value is task type
        edges: List of (task_id, predecessor_id) tuples representing dependencies
        output_path: Path to save the DOT file
    """
    # Build predecessor mapping from edges
    predecessors = defaultdict(list)
    for task_id, pred_id in edges:
        predecessors[task_id].append(pred_id)
    
    # Get all node IDs from task_types
    nodes = set()
    for node_id, task_type in enumerate(task_types):
        if node_id in predecessors or any(pred in task_types for pred in predecessors.get(node_id, [])):
            nodes.add(node_id)
        elif task_type is not None:
            nodes.add(node_id)
    
    # If nodes is empty, try to get nodes from task_types dict
    if not nodes and isinstance(task_types, dict):
        nodes = set(task_types.keys())
    
    # Build DOT content with white background and vertical layout
    dot_lines = [
        'digraph DAG {',
        '  rankdir=TB;',  # Top-to-bottom vertical layout
        '  bgcolor="#ffffff";',
        '  node [fontname="Arial" fontcolor="#333"];',
        '  edge [color="#666" fontname="Arial"];',
        '',
    ]
    
    # Add nodes with black font color
    if isinstance(task_types, (list, tuple)):
        for node_id, task_type in enumerate(task_types):
            if task_type is not None:
                type_name = TASK_TYPE_NAMES.get(task_type, 'UNKNOWN')
                shape_map = {0: 'rect', 1: 'ellipse', 2: 'diamond'}
                shape = shape_map.get(task_type, 'rect')
                color_map = {0: '#ff9800', 1: '#9c27b0', 2: '#00bcd4'}
                color = color_map.get(task_type, '#ff9800')
                dot_lines.append(f'  T{node_id} [label="T{node_id}\\n{type_name}" shape={shape} fillcolor="{color}" fontcolor="#000000"];')
    elif isinstance(task_types, dict):
        for node_id in sorted(task_types.keys()):
            task_type = task_types[node_id]
            type_name = TASK_TYPE_NAMES.get(task_type, 'UNKNOWN')
            shape_map = {0: 'rect', 1: 'ellipse', 2: 'diamond'}
            shape = shape_map.get(task_type, 'rect')
            color_map = {0: '#ff9800', 1: '#9c27b0', 2: '#00bcd4'}
            color = color_map.get(task_type, '#ff9800')
            dot_lines.append(f'  T{node_id} [label="T{node_id}\\n{type_name}" shape={shape} fillcolor="{color}" fontcolor="#000000"];')
    
    dot_lines.append('')
    
    # Add edges
    for task_id, pred_id in sorted(edges):
        dot_lines.append(f'  T{pred_id} -> T{task_id};')
    
    dot_lines.append('}')
    
    # Write to file
    with open(output_path, 'w') as f:
        f.write('\n'.join(dot_lines))
    
    print(f"DOT file saved to: {output_path}")


def render_dot(dot_path, output_path, format='svg'):
    """Render DOT file to image using Graphviz."""
    try:
        # Check if dot command is available
        result = subprocess.run(['which', 'dot'], capture_output=True, text=True)
        if result.returncode != 0:
            print("Warning: Graphviz 'dot' command not found. Install with: brew install graphviz")
            print(f"Generated DOT file: {dot_path}")
            return False
        
        # Render using dot
        cmd = ['dot', '-T', format, '-o', output_path, dot_path]
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode == 0:
            print(f"Rendered image saved to: {output_path}")
            return True
        else:
            print(f"Error rendering DOT: {result.stderr}")
            return False
            
    except Exception as e:
        print(f"Error rendering DOT: {e}")
        return False


def calculate_node_positions(nodes, predecessors, task_types):
    """Calculate node positions for DAG visualization using topological sorting."""
    node_list = sorted(nodes)
    
    # Build DAG structure for topological sorting
    successors = defaultdict(list)
    for task_id, preds in predecessors.items():
        for pred in preds:
            successors[pred].append(task_id)
    
    # Calculate node levels using BFS from roots
    node_levels = {}  # node_id -> level
    roots = []  # nodes with no predecessors
    
    for n in node_list:
        if n not in predecessors or not predecessors[n]:
            roots.append(n)
            node_levels[n] = 0
    
    # If no roots found, use the smallest node id as root
    if not roots:
        roots = [min(node_list)]
        node_levels[roots[0]] = 0
    
    # BFS to assign levels
    queue = list(roots)
    visited = set(roots)
    
    while queue:
        node = queue.pop(0)
        current_level = node_levels[node]
        
        for succ in successors[node]:
            if succ not in visited:
                visited.add(succ)
                node_levels[succ] = current_level + 1
                queue.append(succ)
            else:
                # Update if we found a longer path
                node_levels[succ] = max(node_levels[succ], current_level + 1)
    
    # Group nodes by level
    levels = defaultdict(list)
    for n, lvl in node_levels.items():
        levels[lvl].append(n)
    
    # Calculate positions based on levels (vertical layout - width< height)
    node_positions = {}
    level_spacing_x = 120
    node_spacing_y = 80
    
    # Calculate viewport based on actual node distribution (vertical layout)
    max_nodes_per_level = max(len(nodes_at_level) for nodes_at_level in levels.values()) if levels else 1
    total_width = len(levels) * level_spacing_x + 100
    total_height = max_nodes_per_level * node_spacing_y + 100
    
    for level, nodes_at_level in sorted(levels.items()):
        sorted_nodes = sorted(nodes_at_level)
        x = 60 + level * level_spacing_x
        total_level_height = len(sorted_nodes) * node_spacing_y
        start_y = (total_height - total_level_height) // 2 + 40
        for i, n in enumerate(sorted_nodes):
            y = start_y + i * node_spacing_y
            node_type = task_types.get(n, 0)
            node_positions[n] = {'x': x, 'y': y, 'type': node_type}
    
    return node_positions, total_width, total_height


def generate_svg(nodes, predecessors, task_types, output_path):
    """Generate standalone SVG file with DAG visualization."""
    
    node_positions, total_width, total_height = calculate_node_positions(nodes, predecessors, task_types)
    
    # Node type styles
    type_styles = {
        0: {'shape': 'rect', 'fill': '#ff9800', 'stroke': '#ffb74d', 'label': 'CUBE'},
        1: {'shape': 'ellipse', 'fill': '#9c27b0', 'stroke': '#ce93d8', 'label': 'VECTOR'},
        2: {'shape': 'diamond', 'fill': '#00bcd4', 'stroke': '#4dd0e1', 'label': 'MIX'},
    }
    
    # Categorize nodes by type
    nodes_by_type = defaultdict(list)
    for n in sorted(nodes):
        task_type = task_types.get(n, 0)
        nodes_by_type[task_type].append(n)
    
    # Generate SVG content
    svg_lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {total_width} {total_height}" width="{total_width}" height="{total_height}">',
        '  <defs>',
        '    <marker id="arrowhead" markerWidth="10" markerHeight="7" refX="9" refY="3.5" orient="auto">',
        '      <polygon points="0 0, 10 3.5, 0 7" fill="#666"/>',
        '    </marker>',
        '    <style>',
        '      .node rect, .node ellipse, .node path { filter: drop-shadow(0 2px 4px rgba(0,0,0,0.3)); }',
        '      .node text { font-family: Arial, sans-serif; font-size: 11px; fill: #fff; text-anchor: middle; dominant-baseline: middle; }',
        '      .node .type-label { font-size: 9px; fill: #aaa; }',
        '      .edge { fill: none; stroke: #555; stroke-width: 2; }',
        '    </style>',
        '  </defs>',
        '  <rect width="100%" height="100%" fill="#1a1a2e"/>',
        '  <g id="edges">',
    ]
    
    # Generate edges
    for task_id, preds in sorted(predecessors.items()):
        for pred in sorted(preds):
            if pred in node_positions and task_id in node_positions:
                from_node = node_positions[pred]
                to_node = node_positions[task_id]
                mid_x = (from_node['x'] + to_node['x']) / 2 + 40
                mid_y = (from_node['y'] + to_node['y']) / 2
                svg_lines.append(f'    <path class="edge" d="M {from_node["x"]},{from_node["y"]} Q {mid_x},{mid_y} {to_node["x"]},{to_node["y"]}" marker-end="url(#arrowhead)"/>')
    
    svg_lines.append('</g>')
    svg_lines.append('  <g id="nodes">')
    
    # Generate nodes
    for nid, pos in sorted(node_positions.items()):
        style = type_styles[pos['type']]
        svg_lines.append(f'    <g class="node" transform="translate({pos["x"]}, {pos["y"]})">')
        
        if style['shape'] == 'rect':
            svg_lines.append(f'      <rect x="-45" y="-20" width="90" height="40" rx="6" fill="{style["fill"]}" stroke="{style["stroke"]}" stroke-width="2"/>')
        elif style['shape'] == 'ellipse':
            svg_lines.append(f'      <ellipse rx="45" ry="22" fill="{style["fill"]}" stroke="{style["stroke"]}" stroke-width="2"/>')
        elif style['shape'] == 'diamond':
            svg_lines.append(f'      <path d="M 0,-28 L 45,0 L 0,28 L -45,0 Z" fill="{style["fill"]}" stroke="{style["stroke"]}" stroke-width="2"/>')
        
        svg_lines.append(f'      <text font-weight="bold">T{nid}</text>')
        svg_lines.append(f'      <text class="type-label" y="38">{style["label"]}</text>')
        svg_lines.append('    </g>')
    
    svg_lines.append('  </g>')
    
    # Add legend (positioned at top-left for vertical layout)
    legend_x = 20
    legend_y = 20
    svg_lines.append(f'  <g id="legend" transform="translate({legend_x}, {legend_y})">')
    svg_lines.append('    <rect x="-10" y="-10" width="130" height="100" fill="#1a1a2e" stroke="#333" rx="8"/>')
    svg_lines.append('    <text x="0" y="5" fill="#888" font-size="12">Task Types</text>')
    
    legend_items = [
        (0, 'rect', '#ff9800', 'CUBE (type 0)'),
        (1, 'ellipse', '#9c27b0', 'VECTOR (type 1)'),
        (2, 'diamond', '#00bcd4', 'MIX (type 2)'),
    ]
    
    for i, (type_id, shape, color, label) in enumerate(legend_items):
        y = 25 + i * 25
        if shape == 'rect':
            svg_lines.append(f'    <rect x="0" y="{y-8}" width="20" height="14" rx="2" fill="{color}"/>')
        elif shape == 'ellipse':
            svg_lines.append(f'    <ellipse cx="10" cy="{y-1}" rx="10" ry="7" fill="{color}"/>')
        elif shape == 'diamond':
            svg_lines.append(f'    <path d="M 10,{y-10} L 18,{y-2} L 10,{y+6} L 2,{y-2} Z" fill="{color}"/>')
        svg_lines.append(f'    <text x="30" y="{y+4}" fill="#fff" font-size="11">{label}</text>')
    
    svg_lines.append('  </g>')
    svg_lines.append('</svg>')
    
    with open(output_path, 'w') as f:
        f.write('\n'.join(svg_lines))
    
    total_edges = sum(len(p) for p in predecessors.values())
    print(f"SVG file saved to: {output_path}")
    print(f"Total nodes: {len(node_positions)}, Total edges: {total_edges}")
    print(f"  - CUBE (type 0): {len(nodes_by_type.get(0, []))}")
    print(f"  - VECTOR (type 1): {len(nodes_by_type.get(1, []))}")
    print(f"  - MIX (type 2): {len(nodes_by_type.get(2, []))}")
    
    return len(node_positions), total_edges


def main():
    csv_path = 'esl_proxy/log/pto._thread_0.csv'
    output_path = 'esl_proxy/log/pto._thread_0.dot'
    image_output = output_path.replace('.dot', f'.svg')

    print(f"Parsing: {csv_path}")
    edges, task_types = parse_csv(csv_path)
    
    if not edges:
        print("No dependency edges found in CSV")
        return
    
    print(f"Found {len(edges)} dependency edges")
    print(f"Found {len(task_types)} task type mappings")
    
    # Generate DOT file
    generate_dot(task_types, edges, output_path)
    render_dot(output_path, image_output, 'svg')



if __name__ == '__main__':
    main()