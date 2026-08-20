#!/usr/bin/env python3
"""
Rebuild graphify wiki pages from graph.json / .graphify_extract.json.
Usage: python3 scripts/graphify/update_wiki.py
"""

import json
import sys
from pathlib import Path

from graphify.analyze import god_nodes
from graphify.build import build_from_json
from graphify.cluster import cluster, score_all
from graphify.wiki import to_wiki


def main():
    repo_root = Path(__file__).resolve().parent.parent.parent
    graphify_out = repo_root / "graphify-out"
    extract_file = graphify_out / ".graphify_extract.json"
    graph_json = graphify_out / "graph.json"

    if extract_file.exists():
        extraction = json.loads(extract_file.read_text())
    elif graph_json.exists():
        g_raw = json.loads(graph_json.read_text())
        extraction = {
            "nodes": g_raw.get("nodes", []),
            "edges": g_raw.get("edges", []),
            "hyperedges": g_raw.get("hyperedges", []),
        }
    else:
        print("Error: No extraction file or graph.json found.", file=sys.stderr)
        sys.exit(1)

    G = build_from_json(extraction)

    analysis_file = graphify_out / ".graphify_analysis.json"
    if analysis_file.exists():
        analysis = json.loads(analysis_file.read_text())
        communities = {int(k): v for k, v in analysis["communities"].items()}
        cohesion = {int(k): v for k, v in analysis["cohesion"].items()}
    else:
        communities = cluster(G)
        cohesion = score_all(G, communities)

    labels_file = graphify_out / ".graphify_labels.json"
    labels_raw = json.loads(labels_file.read_text()) if labels_file.exists() else {}
    labels = {int(k): v for k, v in labels_raw.items()}
    gods = god_nodes(G)

    wiki_dir = graphify_out / "wiki"
    n = to_wiki(
        G,
        communities,
        str(wiki_dir),
        community_labels=labels or None,
        cohesion=cohesion,
        god_nodes_data=gods,
    )
    print(f"Wiki successfully rebuilt: {n} articles written to {wiki_dir}/")


if __name__ == "__main__":
    main()
