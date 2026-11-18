import os
from pathlib import Path


def demo_root():
    value = os.environ.get("ECHO_OPEN_DEMO_ROOT")
    if value is None:
        return None
    if any(name in os.environ for name in ("ECHO_DATABASE_URL", "ECHO_MEDIA_ROOT", "ECHO_PUBLIC_SAMPLE_ROOT")):
        raise RuntimeError("公开演示需要独立的数据库和媒体目录")
    if any(name.startswith("ECHO_REVIEW_") for name in os.environ):
        raise RuntimeError("公开演示的外部复核须使用阶跃星辰和获准的公开样例清单")
    directory = Path(value)
    if not directory.is_absolute() or not directory.is_dir() or directory.is_symlink():
        raise RuntimeError("公开演示目录须为独立的绝对路径")
    for name in ("demo.sqlite3", "demo-media"):
        if (directory / name).is_symlink():
            raise RuntimeError("公开演示数据库和媒体目录不可使用符号链接")
    return directory.resolve(strict=True)


ROOT = demo_root()
OPERATOR = "local-operator"
