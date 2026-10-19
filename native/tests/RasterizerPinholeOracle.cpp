// CPU numerical oracle for the shared CUDA pinhole covariance boundary.
// No Qt, image input, GPU or user data is required.
#include "../../gaussian-splatting/submodules/diff-gaussian-rasterization/cuda_rasterizer/pinhole_bounds.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {
bool close(double a, double b, double tolerance = 1e-5) {
  return std::abs(a - b) <= tolerance;
}

bool offsetOracle() {
  // Column-major world->view and world->clip, with W=640, H=360,
  // fx=600, fy=450, cx=100, cy=70. Pixel-border K is explicit.
  const float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  const float vp[16] = {1.875f,0,0,0, 0,2.5f,0,0,
                       -.6875f,-.6111111111f,1,1, 0,0,-.2f,0};
  const auto bounds = gsw_pinhole::covariance_bounds(view, vp, 640.f/1200.f, .4f);
  const double x = .8, z = 1., fx = 600.;
  const double clipped = gsw_pinhole::clamp_coordinate(float(x/z), bounds.min_x, bounds.max_x);
  const double covariance = fx*fx/z/z*.01 + fx*fx*clipped*clipped/z/z;
  const double gradient = 2.*fx*fx*clipped/z/z/z *
      gsw_pinhole::unclamped_gradient(float(x/z), bounds.min_x, bounds.max_x);
  const double pixel = fx*x/z + 100. - .5;
  // Literal, independently worked pinhole J Sigma J^T for diag(.01,.01,1).
  if (!close(pixel, 579.5) || !close(covariance, 234000., .02) ||
      !close(gradient, 576000., .02) || !close(bounds.min_x, -.3266666667) ||
      !close(bounds.max_x, 1.06) || !close(bounds.min_y, -.2755555556) ||
      !close(bounds.max_y, .7644444444)) return false;
  std::printf("offset pixel=%.9f covariance=%.9f gradient=%.9f\n", pixel, covariance, gradient);
  return true;
}

bool centerOracle() {
  const float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  const float vp[16] = {1.875f,0,0,0, 0,2.5f,0,0, 0,0,1,1, 0,0,-.2f,0};
  const auto bounds = gsw_pinhole::covariance_bounds(view, vp, 640.f/1200.f, .4f);
  if (!close(bounds.min_x, -.6933333333) || !close(bounds.max_x, .6933333333) ||
      !close(bounds.min_y, -.52) || !close(bounds.max_y, .52)) return false;
  for (float value : {-.9f, -.2f, .3f, .9f}) {
    const float old = value < -.6933333333f ? -.6933333333f :
                      value > .6933333333f ? .6933333333f : value;
    if (!close(gsw_pinhole::clamp_coordinate(value, bounds.min_x, bounds.max_x), old) ||
        gsw_pinhole::unclamped_gradient(value, bounds.min_x, bounds.max_x) !=
            (value < -.6933333333f || value > .6933333333f ? 0.f : 1.f)) return false;
  }
  std::printf("center unchanged\n");
  return true;
}

bool transformedCameraOracle() {
  // A proper 90 degree Y camera rotation plus translation. Projection-offset
  // recovery must not read vp[8]/vp[9] as if world->clip were camera->clip.
  const float view[16] = {0,0,1,0, 0,1,0,0, -1,0,0,0, 4,-3,2,1};
  const float vp[16] = {-.6875f,-.6111111111f,1,1, 0,2.5f,0,0,
                       -1.875f,0,0,0, 6.125f,-8.7222222222f,1.8f,2};
  const auto bounds = gsw_pinhole::covariance_bounds(view, vp, 640.f/1200.f, .4f);
  if (!close(bounds.min_x, -.3266666667) || !close(bounds.max_x, 1.06) ||
      !close(bounds.min_y, -.2755555556) || !close(bounds.max_y, .7644444444)) return false;
  // Boundary/exterior gradients must use exactly the same asymmetric limits.
  if (gsw_pinhole::unclamped_gradient(bounds.min_x-.01f, bounds.min_x, bounds.max_x) != 0.f ||
      gsw_pinhole::unclamped_gradient(bounds.max_x+.01f, bounds.min_x, bounds.max_x) != 0.f ||
      gsw_pinhole::unclamped_gradient(bounds.min_x, bounds.min_x, bounds.max_x) != 1.f ||
      gsw_pinhole::unclamped_gradient(bounds.max_x, bounds.min_x, bounds.max_x) != 1.f) return false;
  std::printf("transformed camera bounds and gradients\n");
  return true;
}
}

int main() {
  return offsetOracle() && centerOracle() && transformedCameraOracle() ? 0 : 1;
}
