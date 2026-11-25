"""Verify the actual one-file distribution without touching external accounts."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def main():
    project = Path(__file__).resolve().parents[1]
    source = project / "dist-single" / "JazzNfa.exe"
    assert source.is_file(), "Run Build.ps1 first."
    with tempfile.TemporaryDirectory(prefix="Jazz Nfa standalone ") as folder:
        isolated = Path(folder) / "JazzNfa.exe"
        shutil.copyfile(source, isolated)
        assert list(Path(folder).iterdir()) == [isolated]
        env = os.environ.copy()
        env["PATH"] = str(Path(env.get("SystemRoot", env.get("SYSTEMROOT", r"C:\Windows"))) / "System32")
        env["DOTNET_ROOT"] = str(Path(folder) / "no installed dotnet")
        env["DOTNET_MULTILEVEL_LOOKUP"] = "0"
        for launch in (1, 2):
            started = time.monotonic()
            result = subprocess.run([str(isolated), "--self-test"], cwd=folder, env=env, timeout=60)
            assert result.returncode == 0, f"Launch {launch}: bundled service smoke test failed ({result.returncode})."
            assert list(Path(folder).iterdir()) == [isolated], "App created adjacent runtime files."
            print(f"Standalone launch {launch}: passed ({time.monotonic() - started:.2f}s)")
    print("One EXE, self-contained service, pipe handshake, loadout catalog and repeat launch: passed.")


if __name__ == "__main__":
    main()
