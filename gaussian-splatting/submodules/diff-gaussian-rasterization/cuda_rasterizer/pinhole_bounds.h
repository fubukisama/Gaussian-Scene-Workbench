/*
 * Full-pinhole covariance clamp shared by forward/backward CUDA and the CPU
 * numerical oracle. The original centered-camera margin is preserved.
 */
#ifndef GSW_PINHOLE_BOUNDS_H_INCLUDED
#define GSW_PINHOLE_BOUNDS_H_INCLUDED

#if defined(__CUDACC__)
#define GSW_PINHOLE_INLINE __host__ __device__ __forceinline__
#else
#define GSW_PINHOLE_INLINE inline
#endif

namespace gsw_pinhole {

struct CovarianceBounds {
    float min_x, max_x, min_y, max_y;
};

GSW_PINHOLE_INLINE CovarianceBounds covariance_bounds(
    const float* viewmatrix, const float* projmatrix,
    float tan_fovx, float tan_fovy)
{
    // projmatrix is world->clip, not camera->clip. Dot its X/Y rows with
    // the world-space camera forward axis to recover P[0,2] and P[1,2].
    // With pixel-border K these are 2*cx/W-1 and 2*cy/H-1. Translation
    // must not enter this direction-only computation.
    const float offset_x = projmatrix[0] * viewmatrix[2] +
        projmatrix[4] * viewmatrix[6] + projmatrix[8] * viewmatrix[10];
    const float offset_y = projmatrix[1] * viewmatrix[2] +
        projmatrix[5] * viewmatrix[6] + projmatrix[9] * viewmatrix[10];
    return {(-1.3f - offset_x) * tan_fovx, (1.3f - offset_x) * tan_fovx,
            (-1.3f - offset_y) * tan_fovy, (1.3f - offset_y) * tan_fovy};
}

GSW_PINHOLE_INLINE float clamp_coordinate(float value, float lower, float upper)
{
    return value < lower ? lower : value > upper ? upper : value;
}

GSW_PINHOLE_INLINE float unclamped_gradient(float value, float lower, float upper)
{
    return value < lower || value > upper ? 0.f : 1.f;
}

} // namespace gsw_pinhole
#undef GSW_PINHOLE_INLINE
#endif
