from pathlib import Path

Path("bss/merged.patch").write_text(
    "TODO\n" + "\n".join(str(_p) for _p in Path("bss").iterdir())
)
