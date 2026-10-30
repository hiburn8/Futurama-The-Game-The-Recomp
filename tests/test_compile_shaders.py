"""Exercise compiler spelling through actual child processes, without a shader SDK."""

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


class CompilerSelection(unittest.TestCase):
    def test_compiler_spelling(self):
        script = pathlib.Path(__file__).resolve().parents[1] / "tools/compile_shaders.py"
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path_bin = root / "path-bin"
            path_bin.mkdir()
            for compiler, label in ((root / "compiler", "local"), (path_bin / "compiler", "path")):
                compiler.write_text(
                    f"#!{sys.executable}\n"
                    "import pathlib, sys\n"
                    f"pathlib.Path({str(root / 'selection')!r}).open('a')"
                    f".write({label!r} + '\\n')\n"
                    "pathlib.Path(sys.argv[sys.argv.index('-o') + 1]).write_bytes(b'DXBCfixture')\n"
                )
                compiler.chmod(0o700)
            environment = dict(os.environ, PATH=str(path_bin))
            for index, (argument, expected) in enumerate(
                (("./compiler", "local"), (str(root / "compiler"), "local"), ("compiler", "path"))
            ):
                (root / "selection").write_text("")
                output = root / f"out-{index}"
                subprocess.run(
                    [sys.executable, str(script), "--compiler", argument, "--out", str(output)],
                    cwd=root,
                    env=environment,
                    check=True,
                    timeout=15,
                )
                self.assertEqual((root / "selection").read_text().splitlines(), [expected] * 4)
                self.assertEqual(len(list(output.glob("*.dxbc"))), 4)


if __name__ == "__main__":
    unittest.main()
