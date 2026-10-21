#!/usr/bin/env python3
"""Prepare a compact AOWIS country-boundary GeoJSON from a local Natural Earth GeoJSON.

No network access and no Python dependencies. Supply a .geojson/.json file or a ZIP
containing a GeoJSON file, and copy the resulting regions.geojson to the server.
"""
import argparse
import json
import pathlib
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path, help="Natural Earth GeoJSON or ZIP containing it")
    parser.add_argument("destination", type=pathlib.Path, help="Output regions.geojson")
    args = parser.parse_args()
    if args.source.suffix.lower() == ".zip":
        with zipfile.ZipFile(args.source) as archive:
            names = [name for name in archive.namelist() if name.lower().endswith((".geojson", ".json"))]
            if len(names) != 1:
                parser.error("ZIP must contain exactly one GeoJSON file (not an ESRI Shapefile)")
            raw = archive.read(names[0])
    else:
        raw = args.source.read_bytes()
    document = json.loads(raw)
    if document.get("type") != "FeatureCollection":
        parser.error("Source must be a GeoJSON FeatureCollection")
    result = {"type": "FeatureCollection", "features": []}
    used = set()
    for feature in document.get("features", []):
        properties = feature.get("properties") or {}
        code = properties.get("ISO_A2")
        if not code or code == "-99":
            code = properties.get("ISO_A2_EH") or properties.get("id")
        geometry = feature.get("geometry") or {}
        if not isinstance(code, str) or not code or code == "-99":
            continue
        if geometry.get("type") not in ("Polygon", "MultiPolygon"):
            continue
        if code in used:
            # Multiple features can share a code: merge through MultiPolygon instead.
            existing = next(f for f in result["features"] if f["properties"]["id"] == code)
            current = existing["geometry"]
            polygons = current["coordinates"] if current["type"] == "MultiPolygon" else [current["coordinates"]]
            polygons += geometry["coordinates"] if geometry["type"] == "MultiPolygon" else [geometry["coordinates"]]
            current.update(type="MultiPolygon", coordinates=polygons)
            continue
        used.add(code)
        result["features"].append({"type": "Feature", "properties": {"id": code}, "geometry": geometry})
    if not result["features"]:
        parser.error("No features with country IDs were found")
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_text(json.dumps(result, separators=(",", ":")), encoding="utf-8")
    print(f"Prepared {len(result['features'])} regions: {args.destination}")


if __name__ == "__main__":
    main()
