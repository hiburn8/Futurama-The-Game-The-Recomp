#!/usr/bin/env python3
"""Compile the existing renderer HLSL to DXBC using native vkd3d-compiler."""

import argparse
import pathlib
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()
    source = (
        pathlib.Path(__file__).resolve().parents[1] / "src/render/xd3d/D3D11Device.cpp"
    )
    text = source.read_text()
    shader = text.split('const char kShader[] = R"(', 1)[1].split(')";', 1)[0]
    args.out.mkdir(parents=True, exist_ok=True)
    hlsl = args.out / "fixedfunction.hlsl"
    hlsl.write_text(shader)
    for entry, profile in (
        ("VSMain", "vs_4_0"),
        ("PSMain", "ps_4_0"),
        ("ClearVS", "vs_4_0"),
        ("ClearPS", "ps_4_0"),
    ):
        output = args.out / f"{entry}.dxbc"
        temporary = args.out / f"{entry}.dxbc.tmp"
        subprocess.run(
            [
                args.compiler,
                "-x",
                "hlsl",
                "-b",
                "dxbc-tpf",
                "-p",
                profile,
                "-e",
                entry,
                "-o",
                str(temporary),
                str(hlsl),
            ],
            check=True,
        )
        if temporary.read_bytes()[:4] != b"DXBC":
            raise ValueError(f"Compiler did not produce DXBC for {entry}")
        temporary.replace(output)


if __name__ == "__main__":
    main()
