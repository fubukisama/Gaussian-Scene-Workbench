"""Opt-in single-Gaussian full-pinhole covariance/gradient acceptance.

Uses the public GaussianRasterizer, not the native preview. CPU oracle values
come from the pinhole Jacobian and closed-form Gaussian density. No user scene,
image or training checkpoint is discovered. --cpu-only does not import Torch.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import uuid

import numpy as np


WIDTH, HEIGHT, FX, FY = 640, 360, 600., 450.
OPACITY = .5


def unpack_covariance(packed):
    xx, xy, xz, yy, yz, zz = np.asarray(packed, dtype=np.float64)
    return np.asarray([[xx, xy, xz], [xy, yy, yz], [xz, yz, zz]])


def pack_covariance(matrix):
    return np.asarray([matrix[0, 0], matrix[0, 1], matrix[0, 2],
                       matrix[1, 1], matrix[1, 2], matrix[2, 2]], dtype=np.float64)


def oracle(mean, covariance, rotation, translation, cx, cy, samples):
    """Independent double-precision EWA value and analytic world gradients."""
    mean = np.asarray(mean, dtype=np.float64)
    rotation = np.asarray(rotation, dtype=np.float64)
    translation = np.asarray(translation, dtype=np.float64)
    x, y, z = point = rotation @ mean + translation
    if z <= 0:
        raise ValueError("oracle needs positive camera depth")
    camera_covariance = rotation @ unpack_covariance(covariance) @ rotation.T
    jacobian = np.asarray([[FX/z, 0., -FX*x/(z*z)], [0., FY/z, -FY*y/(z*z)]])
    image_covariance = jacobian @ camera_covariance @ jacobian.T + np.eye(2) * .3
    inverse = np.linalg.inv(image_covariance)
    center = np.asarray([FX*x/z + cx - .5, FY*y/z + cy - .5])
    derivatives = (
        np.asarray([[0., 0., -FX/(z*z)], [0., 0., 0.]]),
        np.asarray([[0., 0., 0.], [0., 0., -FY/(z*z)]]),
        np.asarray([[-FX/(z*z), 0., 2*FX*x/(z**3)], [0., -FY/(z*z), 2*FY*y/(z**3)]]),
    )
    gradient_center = np.zeros(2)
    gradient_covariance = np.zeros((2, 2))
    values = []
    for sample in samples:
        delta = np.asarray(sample, dtype=np.float64) - center
        ray = inverse @ delta
        value = OPACITY * math.exp(-.5 * float(delta @ ray))
        values.append(value)
        gradient_center += value * ray / len(samples)
        gradient_covariance += .5 * value * np.outer(ray, ray) / len(samples)
    camera_gradient = jacobian.T @ gradient_center
    for axis, derivative in enumerate(derivatives):
        derivative_covariance = derivative @ camera_covariance @ jacobian.T + jacobian @ camera_covariance @ derivative.T
        camera_gradient[axis] += float(np.sum(gradient_covariance * derivative_covariance))
    a = jacobian @ rotation
    covariance_gradient = a.T @ gradient_covariance @ a
    packed_gradient = pack_covariance(covariance_gradient)
    packed_gradient[[1, 2, 4]] *= 2.
    return {"center": center.tolist(), "camera_point": point.tolist(),
            "image_covariance": image_covariance.tolist(), "values": values,
            "loss": float(np.mean(values)), "mean_gradient": (rotation.T @ camera_gradient).tolist(),
            "covariance_gradient": packed_gradient.tolist()}


def cases():
    camera_mean = np.asarray([.8, 0., 1.])
    camera_covariance = np.diag([1e-4, 1e-4, 1e-2])
    rotation = np.asarray([[0., 0., -1.], [0., 1., 0.], [1., 0., 0.]])
    translation = np.asarray([4., -3., 2.])
    return [
        {"name": "offset_identity", "mean": camera_mean, "covariance": pack_covariance(camera_covariance),
         "rotation": np.eye(3), "translation": np.zeros(3), "cx": 100., "cy": 180.,
         "samples": [(540, 180), (619, 180)], "ratio_pixels": [(540, 180), (580, 180)]},
        {"name": "centered_regression", "mean": np.asarray([.2, 0., 1.]), "covariance": pack_covariance(camera_covariance),
         "rotation": np.eye(3), "translation": np.zeros(3), "cx": 320., "cy": 180.,
         "samples": [(400, 180), (479, 180)], "ratio_pixels": [(400, 180), (440, 180)]},
        {"name": "offset_rotated_translated_view", "mean": rotation.T @ (camera_mean - translation),
         "covariance": pack_covariance(rotation.T @ camera_covariance @ rotation),
         "rotation": rotation, "translation": translation, "cx": 100., "cy": 180.,
         "samples": [(540, 180), (619, 180)], "ratio_pixels": [(540, 180), (580, 180)]},
    ]


def numeric_cpu_gradient(case, field, epsilon):
    values = np.asarray(case[field], dtype=np.float64)
    gradient = np.zeros_like(values)
    for index in range(len(values)):
        plus, minus = values.copy(), values.copy()
        plus[index] += epsilon
        minus[index] -= epsilon
        arguments = {key: case[key] for key in ("mean", "covariance", "rotation", "translation", "cx", "cy", "samples")}
        arguments[field] = plus
        upper = oracle(**arguments)["loss"]
        arguments[field] = minus
        lower = oracle(**arguments)["loss"]
        gradient[index] = (upper - lower) / (2. * epsilon)
    return gradient


def cpu_self_check():
    evidence = []
    for case in cases():
        expected = oracle(**{key: case[key] for key in ("mean", "covariance", "rotation", "translation", "cx", "cy", "samples")})
        mean_difference = numeric_cpu_gradient(case, "mean", 1e-6)
        covariance_difference = numeric_cpu_gradient(case, "covariance", 1e-8)
        np.testing.assert_allclose(expected["mean_gradient"], mean_difference, atol=1e-7, rtol=2e-6)
        np.testing.assert_allclose(expected["covariance_gradient"], covariance_difference, atol=1e-6, rtol=2e-6)
        if case["name"] == "offset_identity":
            np.testing.assert_allclose(expected["center"], [579.5, 179.5], atol=1e-12)
            assert abs(expected["image_covariance"][0][0] - 2340.3) < 1e-9
            # dCxx/d(camera_x) = 2*fx^2*x*Sigma_zz = 5760.
            assert abs(2 * FX * FX * .8 * .01 - 5760.) < 1e-9
        evidence.append(dict(name=case["name"], **expected,
                             cpu_finite_mean_gradient=mean_difference.tolist(),
                             cpu_finite_covariance_gradient=covariance_difference.tolist()))
    return evidence


def projection(cx, cy):
    matrix = np.zeros((4, 4), dtype=np.float64)
    matrix[0, 0], matrix[1, 1] = 2*FX/WIDTH, 2*FY/HEIGHT
    matrix[0, 2], matrix[1, 2] = 2*cx/WIDTH-1, 2*cy/HEIGHT-1
    matrix[2, 2], matrix[2, 3], matrix[3, 2] = 50./49.8, -10./49.8, 1.
    return matrix


def run_case(torch, module, case, mean_epsilon, covariance_epsilon):
    view = np.eye(4)
    view[:3, :3], view[:3, 3] = case["rotation"], case["translation"]
    world_to_clip = projection(case["cx"], case["cy"]) @ view
    center = -case["rotation"].T @ case["translation"]
    settings = module.GaussianRasterizationSettings(image_height=HEIGHT, image_width=WIDTH,
        tanfovx=WIDTH/(2*FX), tanfovy=HEIGHT/(2*FY),
        bg=torch.zeros(3, device="cuda"), scale_modifier=1.,
        viewmatrix=torch.tensor(view.T.copy(), dtype=torch.float32, device="cuda"),
        projmatrix=torch.tensor(world_to_clip.T.copy(), dtype=torch.float32, device="cuda"),
        sh_degree=0, campos=torch.tensor(center, dtype=torch.float32, device="cuda"),
        prefiltered=False, debug=False, antialiasing=False)
    rasterizer = module.GaussianRasterizer(raster_settings=settings)

    def render(mean, covariance, gradients=False):
        mean_tensor = torch.tensor(np.asarray(mean)[None], dtype=torch.float32, device="cuda", requires_grad=gradients)
        covariance_tensor = torch.tensor(np.asarray(covariance)[None], dtype=torch.float32, device="cuda", requires_grad=gradients)
        screen = torch.zeros_like(mean_tensor, requires_grad=gradients)
        result = rasterizer(means3D=mean_tensor, means2D=screen,
            colors_precomp=torch.tensor([[1., 0., 0.]], dtype=torch.float32, device="cuda"),
            opacities=torch.tensor([[OPACITY]], dtype=torch.float32, device="cuda"),
            cov3D_precomp=covariance_tensor)
        image, radii = result[0], result[1]
        loss = sum(image[0, y, x] for x, y in case["samples"]) / len(case["samples"])
        if gradients:
            loss.backward()
        torch.cuda.synchronize()
        return loss, image, radii, mean_tensor, covariance_tensor

    expected = oracle(**{key: case[key] for key in ("mean", "covariance", "rotation", "translation", "cx", "cy", "samples")})
    loss, image, radii, mean_tensor, covariance_tensor = render(case["mean"], case["covariance"], True)
    if not bool((radii > 0).all()) or not bool(torch.isfinite(image).all()):
        raise RuntimeError("single Gaussian was not rendered with finite pixels: " + case["name"])
    values = [float(image[0, y, x]) for x, y in case["samples"]]
    ratio_values = [float(image[0, y, x]) for x, y in case["ratio_pixels"]]
    mx = expected["center"][0]
    difference = (case["ratio_pixels"][0][0]-mx)**2 - (case["ratio_pixels"][1][0]-mx)**2
    if min(ratio_values) <= 0 or ratio_values[0] == ratio_values[1]:
        raise RuntimeError("invalid single-Gaussian covariance ratio sample")
    inferred = -.5 * difference / math.log(ratio_values[0] / ratio_values[1])
    mean_gradient = mean_tensor.grad.detach().cpu().numpy()[0].astype(np.float64)
    covariance_gradient = covariance_tensor.grad.detach().cpu().numpy()[0].astype(np.float64)
    finite = {}
    for field, epsilon in (("mean", mean_epsilon), ("covariance", covariance_epsilon)):
        vector = np.asarray(case[field], dtype=np.float64)
        gradient = np.zeros_like(vector)
        for index in range(len(vector)):
            plus, minus = vector.copy(), vector.copy()
            plus[index] += epsilon
            minus[index] -= epsilon
            args = {"mean": case["mean"], "covariance": case["covariance"]}
            args[field] = plus
            with torch.no_grad():
                upper = float(render(args["mean"], args["covariance"])[0])
                args[field] = minus
                lower = float(render(args["mean"], args["covariance"])[0])
            gradient[index] = (upper-lower)/(2*epsilon)
        finite[field] = gradient
    checks = {
        "pixel_values": bool(np.allclose(values, expected["values"], rtol=3e-4, atol=2e-6)),
        "covariance_from_pixel_ratio": abs(inferred-expected["image_covariance"][0][0]) <= .2,
        "mean_gradient_analytic": bool(np.allclose(mean_gradient, expected["mean_gradient"], rtol=.003, atol=.004)),
        "covariance_gradient_analytic": bool(np.allclose(covariance_gradient, expected["covariance_gradient"], rtol=.003, atol=.004)),
        "mean_gradient_gpu_difference": bool(np.allclose(mean_gradient, finite["mean"], rtol=.02, atol=.007)),
        "covariance_gradient_gpu_difference": bool(np.allclose(covariance_gradient, finite["covariance"], rtol=.02, atol=.025)),
    }
    return {"name": case["name"], "accepted": all(checks.values()), "checks": checks,
            "expected": expected, "observed": {"loss": float(loss.detach()), "values": values,
                "ratio_pixels": case["ratio_pixels"], "ratio_values": ratio_values,
                "inferred_cxx": inferred, "mean_gradient": mean_gradient.tolist(),
                "covariance_gradient": covariance_gradient.tolist(),
                "finite_mean_gradient": finite["mean"].tolist(),
                "finite_covariance_gradient": finite["covariance"].tolist(), "radii": radii.cpu().tolist()},
            "finite_difference_steps": {"mean": mean_epsilon, "covariance": covariance_epsilon}}


def write_result(path, data):
    path = Path(path).resolve()
    if path.drive.upper() != "D:":
        raise ValueError("Acceptance results must be written on D:")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(data, stream, sort_keys=True, ensure_ascii=False, allow_nan=False, indent=2)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-root", help="Isolated directory containing diff_gaussian_rasterization")
    parser.add_argument("--expect", choices=("stock", "fixed"), default="fixed")
    parser.add_argument("--output", required=True, help="New JSON evidence path on D:")
    parser.add_argument("--cpu-only", action="store_true", help="Verify independent numerical oracle without importing Torch")
    parser.add_argument("--mean-eps", type=float, default=2e-4)
    parser.add_argument("--cov-eps", type=float, default=2e-6)
    args = parser.parse_args(argv)
    evidence = {"schema": "gsw.pinhole_rasterizer_acceptance/1", "expected_runtime": args.expect,
                "gpu_executed": False, "accepted": False, "scope": "single_gaussian_numeric_not_geometry_accuracy"}
    try:
        if not 0 < args.mean_eps <= .001 or not 0 < args.cov_eps <= 5e-6:
            raise ValueError("finite-difference steps exceed the bounded oracle domain")
        evidence["cpu_oracle"] = cpu_self_check()
        if args.cpu_only:
            evidence.update(accepted=True, oracle_only=True)
            write_result(args.output, evidence)
            print(json.dumps({"accepted": True, "gpu_executed": False, "output": args.output}))
            return 0
        if args.module_root:
            root = Path(args.module_root).resolve()
            if root.name == "diff_gaussian_rasterization":
                root = root.parent
            sys.path.insert(0, str(root))
        import torch
        import diff_gaussian_rasterization as module
        binary = Path(module._C.__file__).resolve()
        if args.module_root:
            binary.relative_to(root)  # Fail if an older installed extension won import selection.
        marker = getattr(module._C, "GSW_PINHOLE_COVARIANCE_VERSION", None)
        evidence["runtime"] = {"package": str(Path(module.__file__).resolve()), "binary": str(binary),
            "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "pinhole_covariance_version": marker,
            "torch": torch.__version__, "cuda_runtime": torch.version.cuda}
        if (args.expect == "fixed" and marker != 1) or (args.expect == "stock" and marker is not None):
            raise ValueError("selected extension does not match --expect runtime identity")
        if not torch.cuda.is_available():
            raise RuntimeError("A real CUDA device is required; CPU/preview results are not GPU acceptance")
        evidence["runtime"]["device"] = torch.cuda.get_device_name()
        evidence["gpu_executed"] = True
        evidence["cases"] = [run_case(torch, module, case, args.mean_eps, args.cov_eps) for case in cases()]
        evidence["accepted"] = all(case["accepted"] for case in evidence["cases"])
        if args.expect == "stock":
            offset = evidence["cases"][0]
            evidence["known_stock_cxx"] = 1766.86
            evidence["stock_regression_reproduced"] = (not offset["checks"]["covariance_from_pixel_ratio"] and
                abs(offset["observed"]["inferred_cxx"] - 1766.86) < .2)
            evidence["expected_failure"] = bool(evidence["stock_regression_reproduced"])
            # Stock is intentionally RED against the physically correct oracle.
            evidence["accepted"] = False
        write_result(args.output, evidence)
        print(json.dumps({"accepted": evidence["accepted"], "expected_failure": evidence.get("expected_failure", False),
                          "output": args.output}, ensure_ascii=False))
        return 0 if evidence["accepted"] else 1
    except Exception as error:
        evidence["error"] = {"type": type(error).__name__, "message": str(error)}
        write_result(args.output, evidence)
        print(json.dumps(evidence["error"], ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
