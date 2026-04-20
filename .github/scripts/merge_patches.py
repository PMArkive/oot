from pathlib import Path

Path("fix_bss_merged.patch").write_text(
    "TODO\n" + "\n".join(str(_p) for _p in Path(".").glob("fix_bss_*.patch"))
)
