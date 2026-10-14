# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Backend-appropriate surfel regularization without inactive autograd work."""


def surfel_regularization(torch, render_pkg, normal_weight, distortion_weight):
    """Return already-weighted ``(normal_loss, distortion_loss)`` scalars.

    An inactive term requires no corresponding map and has no autograd graph.
    Active terms retain the existing FP32 multiplication/reduction order. The
    RGB render supplies the zero scalar's device/dtype, not an artificial model
    dependency; the photometric objective still owns the model's image gradient.
    This does not suppress the rasterizer's auxiliary-map forward computation.
    """
    if normal_weight == 0 and distortion_weight == 0:
        image = render_pkg["render"]
        return image.new_zeros(()), image.new_zeros(())
    if normal_weight == 0:
        normal_loss = render_pkg["render"].new_zeros(())
    else:
        normal_error = (1 - (render_pkg["rend_normal"] * render_pkg["surf_normal"]).sum(dim=0))[None]
        normal_loss = normal_weight * normal_error.mean()
    distortion_loss = (render_pkg["render"].new_zeros(()) if distortion_weight == 0 else
                       distortion_weight * render_pkg["rend_dist"].mean())
    return normal_loss, distortion_loss
