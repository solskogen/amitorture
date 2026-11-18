from pathlib import Path

# scratch space some tests put their temporary directories in (gitignored;
# missing on a fresh clone)
(Path(__file__).resolve().parents[2] / "work" / "tmp").mkdir(parents=True, exist_ok=True)
