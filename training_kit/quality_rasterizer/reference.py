"""Small double-precision numerical oracle, independent of Torch/CUDA."""
import math


def aggregate_screen_gradients(per_pixel):
    signed, absolute = [0., 0.], [0., 0.]
    for gradient in per_pixel:
        if len(gradient) != 2 or not all(math.isfinite(float(x)) for x in gradient):
            raise ValueError('quality_reference_gradient_invalid')
        for axis in range(2):
            signed[axis] += float(gradient[axis])
            absolute[axis] += abs(float(gradient[axis]))
    return {'signed': signed, 'absolute': absolute}


def composite_pixel(alphas, inverse_depths, colors, background=(0., 0., 0.)):
    """Depth-ordered scalar oracle with the rasterizer's acceptance rules."""
    if not len(alphas) == len(inverse_depths) == len(colors):
        raise ValueError('quality_reference_shape_mismatch')
    transmittance, moment = 1., 0.
    rgb, counts = [0., 0., 0.], [0] * len(alphas)
    for index, (raw_alpha, invz, color) in enumerate(zip(alphas, inverse_depths, colors)):
        if not all(math.isfinite(float(x)) for x in [raw_alpha, invz] + list(color)):
            raise ValueError('quality_reference_nonfinite')
        alpha = min(.99, float(raw_alpha))
        if alpha < 1. / 255.:
            continue
        tentative = transmittance * (1. - alpha)
        if tentative < 1e-4:
            break
        weight = transmittance * alpha
        for channel in range(3):
            rgb[channel] += weight * color[channel]
        moment += weight * invz
        counts[index] = 1
        transmittance = tentative
    for channel in range(3):
        rgb[channel] += transmittance * background[channel]
    return {'rgb': rgb, 'alpha': 1. - transmittance,
            'inverse_depth_moment': moment, 'contributor_pixels': counts}
