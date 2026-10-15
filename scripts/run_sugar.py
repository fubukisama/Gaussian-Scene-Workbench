import runpy
import sys
import types
from pathlib import Path


def install_eager_dynamo_shim():
    import torch

    dynamo = types.ModuleType("torch._dynamo")

    def disable(function=None, *args, **kwargs):
        if function is None:
            return lambda wrapped: wrapped
        return function

    dynamo.disable = disable
    dynamo.graph_break = lambda *args, **kwargs: None
    dynamo.is_compiling = lambda: False
    sys.modules["torch._dynamo"] = dynamo
    torch._dynamo = dynamo
    return torch


def probe_runtime():
    torch = install_eager_dynamo_shim()
    import open3d
    import pytorch3d

    parameter = torch.nn.Parameter(torch.zeros(1))
    optimizer = torch.optim.Adam([parameter])
    parameter.sum().backward()
    optimizer.step()
    optimizer.zero_grad()
    print("torch", torch.__version__)
    print("pytorch3d", getattr(pytorch3d, "__version__", "unknown"))
    print("open3d", open3d.__version__)
    print("adam", "ok")


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args == ["--probe"]:
        probe_runtime()
        return 0
    if not args:
        raise SystemExit("Usage: run_sugar.py <SuGaR train.py> [arguments...]")

    entry = Path(args[0]).resolve()
    if not entry.is_file():
        raise SystemExit(f"Missing SuGaR entry point: {entry}")
    install_eager_dynamo_shim()
    entry_parent = str(entry.parent)
    if entry_parent not in sys.path:
        sys.path.insert(0, entry_parent)
    sys.argv = [str(entry), *args[1:]]
    runpy.run_path(str(entry), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
