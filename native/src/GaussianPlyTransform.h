#pragma once

#include "ModelExport.h"

#include <QCoreApplication>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace gsw::detail {

struct GaussianBakeValues {
  std::array<double, 3> logScale{};
  std::array<double, 4> quaternion{}; // w, x, y, z
  QVector<double> sh;              // channel-major f_rest; DC is unchanged.
  int dimensions = 3;
};

// Independent double-precision implementation of Gaussian affine covariance
// transforms and real spherical-harmonic basis changes. No training code is
// linked or copied. The real SH convention is m=-l..l with Condon-Shortley phase:
// degree one is sqrt(3/(4*pi)) * [-y, z, -x], as in the existing renderer.
class GaussianPlyTransform {
public:
  explicit GaussianPlyTransform(const ModelExportOptions &options) {
    const auto &s = options.transform.scale;
    const auto &q = options.transform.rotation;
    mValid = options.transform.isValid() &&
        std::isfinite(options.sceneUnitScale) && options.sceneUnitScale > 0.0;
    if (!mValid) return;
    const Vector scale{double(s.x()), double(s.y()), double(s.z())};
    Matrix rotation{};
    if (!rotationMatrix({double(q.scalar()), double(q.x()), double(q.y()),
                         double(q.z())}, rotation)) {
      mValid = false;
      return;
    }
    mConformal = std::abs(scale[0]) == std::abs(scale[1]) &&
                 std::abs(scale[0]) == std::abs(scale[2]);
    mUniformFactor = options.sceneUnitScale * std::abs(scale[0]);
    mLinearIdentity = q.x() == 0.0F && q.y() == 0.0F && q.z() == 0.0F;
    mAngularIdentity = mLinearIdentity && scale[0] > 0.0 &&
                      scale[1] > 0.0 && scale[2] > 0.0;
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        mA[row][col] = rotation[row][col] * scale[col] * options.sceneUnitScale;
        mQ[row][col] = rotation[row][col] * std::copysign(1.0, scale[col]);
        mValid = mValid && std::isfinite(mA[row][col]);
      }
      mLinearIdentity = mLinearIdentity &&
          scale[row] * options.sceneUnitScale == 1.0;
    }
    mValid = mValid && std::isfinite(mUniformFactor) && mUniformFactor > 0.0;
    if (mValid && mConformal && !mAngularIdentity) prepareShTransform();
  }

  bool apply(GaussianBakeValues &values, QString &error) const {
    const int rest = values.sh.size() <= 72 && values.sh.size() % 3 == 0
        ? static_cast<int>(values.sh.size() / 3) : -1;
    int degree = -1;
    for (int l = 0; l <= 4; ++l)
      if (rest == (l + 1) * (l + 1) - 1) degree = l;
    if ((values.dimensions != 2 && values.dimensions != 3) || degree < 0) {
      error = schemaError();
      return false;
    }
    if (!mValid) return invalid(error);
    bool directional = false;
    for (double coefficient : values.sh) {
      if (!std::isfinite(coefficient)) return invalid(error);
      directional = directional || coefficient != 0.0;
    }
    Matrix sourceRotation{};
    if (!rotationMatrix(values.quaternion, sourceRotation)) return invalid(error);
    Vector scales{};
    for (int col = 0; col < values.dimensions; ++col) {
      if (!std::isfinite(values.logScale[col])) return invalid(error);
      scales[col] = std::exp(values.logScale[col]);
      if (!std::isfinite(scales[col]) || scales[col] <= 0.0)
        return invalid(error);
    }
    // Translation alone must not normalize/rewrite otherwise untouched source
    // fields. The caller continues copying their original bytes/tokens.
    if (mLinearIdentity) return true;
    if (!mConformal && directional) {
      error = QCoreApplication::translate("Workbench", "Non-uniform Gaussian scaling with non-zero directional SH cannot preserve appearance exactly. Use uniform scaling or export original coordinates.");
      return false;
    }

    auto resultLogScale = values.logScale;
    auto resultQuaternion = values.quaternion;
    std::array<double, 72> resultSh{};
    Matrix outputRotation{};
    if (mConformal) {
      // Keep original principal-axis ordering for similarities, including
      // signed scales. Reflections are absorbed by an arbitrary column sign;
      // column signs do not affect a 3D Gaussian's covariance.
      outputRotation = multiply(mQ, sourceRotation);
      if (values.dimensions == 2) {
        const Vector normal = cross(column(outputRotation, 0),
                                    column(outputRotation, 1));
        setColumn(outputRotation, 2, normal);
      } else if (determinant(outputRotation) < 0.0) {
        for (int row = 0; row < 3; ++row) outputRotation[row][2] *= -1.0;
      }
      const double logFactor = std::log(mUniformFactor);
      for (int col = 0; col < values.dimensions; ++col)
        resultLogScale[col] += logFactor;
    } else {
      Matrix factor = multiply(mA, sourceRotation);
      double normalization = 0.0;
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < values.dimensions; ++col) {
          factor[row][col] *= scales[col];
          if (!std::isfinite(factor[row][col])) return invalid(error);
          normalization = std::max(normalization, std::abs(factor[row][col]));
        }
      }
      if (!std::isfinite(normalization) || normalization <= 0.0)
        return invalid(error);
      for (int row = 0; row < 3; ++row)
        for (int col = 0; col < values.dimensions; ++col)
          factor[row][col] /= normalization;
      // One-sided Jacobi SVD of A R diag(s), rather than eigenvalues of
      // B B^T: forming that covariance squares the condition number and can
      // silently erase a very thin Gaussian. Right rotations preserve the
      // latent isotropic Gaussian, for both 3D ellipsoids and 2D disks.
      for (int sweep = 0; sweep < 64; ++sweep) {
        bool changed = false;
        for (int p = 0; p < values.dimensions; ++p) {
          for (int q = p + 1; q < values.dimensions; ++q) {
            const Vector a = column(factor, p), b = column(factor, q);
            const double alpha = dot(a, a), beta = dot(b, b), gamma = dot(a, b);
            if (!std::isfinite(alpha) || !std::isfinite(beta) ||
                !std::isfinite(gamma) || alpha <= 0.0 || beta <= 0.0)
              return invalid(error);
            const double threshold = 4.0 * std::numeric_limits<double>::epsilon() *
                std::sqrt(alpha) * std::sqrt(beta);
            if (std::abs(gamma) <= threshold) continue;
            const double tau = (beta - alpha) / (2.0 * gamma);
            const double tangent = std::copysign(1.0, tau) /
                (std::abs(tau) + std::hypot(1.0, tau));
            const double cosine = 1.0 / std::hypot(1.0, tangent);
            const double sine = cosine * tangent;
            for (int row = 0; row < 3; ++row) {
              factor[row][p] = cosine * a[row] - sine * b[row];
              factor[row][q] = sine * a[row] + cosine * b[row];
            }
            changed = true;
          }
        }
        if (!changed) break;
      }
      for (int col = 0; col < values.dimensions; ++col) {
        const Vector axis = column(factor, col);
        const double length = std::hypot(axis[0], axis[1], axis[2]);
        if (!std::isfinite(length) || length <= 0.0) return invalid(error);
        resultLogScale[col] = std::log(normalization) + std::log(length);
        setColumn(outputRotation, col,
                  {axis[0] / length, axis[1] / length, axis[2] / length});
      }
      if (values.dimensions == 2) {
        setColumn(outputRotation, 2,
                  cross(column(outputRotation, 0), column(outputRotation, 1)));
      } else if (determinant(outputRotation) < 0.0) {
        for (int row = 0; row < 3; ++row) outputRotation[row][2] *= -1.0;
      }
    }
    if (!(mConformal && mAngularIdentity) &&
        !quaternionFromRotation(outputRotation, resultQuaternion))
      return invalid(error);
    for (int col = 0; col < values.dimensions; ++col) {
      const double activated = std::exp(resultLogScale[col]);
      if (!std::isfinite(resultLogScale[col]) || !std::isfinite(activated) ||
          activated <= 0.0) return invalid(error);
    }
    if (directional && !mAngularIdentity) {
      for (int channel = 0; channel < 3; ++channel) {
        for (int l = 1; l <= degree; ++l) {
          for (int j = l * l; j < (l + 1) * (l + 1); ++j) {
            double sum = 0.0;
            for (int i = l * l; i < (l + 1) * (l + 1); ++i)
              sum += mSh[j][i] * values.sh[channel * rest + i - 1];
            if (!std::isfinite(sum)) return invalid(error);
            resultSh[channel * rest + j - 1] = sum;
          }
        }
      }
    }
    values.logScale = resultLogScale;
    values.quaternion = resultQuaternion;
    if (directional && !mAngularIdentity)
      for (qsizetype i = 0; i < values.sh.size(); ++i) values.sh[i] = resultSh[i];
    return true;
  }

private:
  using Vector = std::array<double, 3>;
  using Matrix = std::array<Vector, 3>; // row-major
  static constexpr double Pi = 3.141592653589793238462643383279502884;
  bool mValid = false;
  bool mConformal = false;
  bool mLinearIdentity = false;
  bool mAngularIdentity = false;
  double mUniformFactor = 1.0;
  Matrix mA{}, mQ{};
  std::array<std::array<double, 25>, 25> mSh{};

  static QString schemaError() {
    return QCoreApplication::translate("Workbench", "Gaussian transform baking requires complete floating-point scale, quaternion and SH fields (degree 0–4).");
  }
  static bool invalid(QString &error) {
    error = QCoreApplication::translate("Workbench", "Gaussian transform baking encountered invalid or unrepresentable scale, rotation or SH values. No output was written.");
    return false;
  }
  static double dot(const Vector &a, const Vector &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  }
  static Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
  }
  static Vector column(const Matrix &m, int col) {
    return {m[0][col], m[1][col], m[2][col]};
  }
  static void setColumn(Matrix &m, int col, const Vector &v) {
    for (int row = 0; row < 3; ++row) m[row][col] = v[row];
  }
  static Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix result{};
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        for (int k = 0; k < 3; ++k) result[row][col] += a[row][k] * b[k][col];
    return result;
  }
  static double determinant(const Matrix &m) {
    return dot(column(m, 0), cross(column(m, 1), column(m, 2)));
  }
  static bool rotationMatrix(const std::array<double, 4> &q, Matrix &r) {
    for (double value : q) if (!std::isfinite(value)) return false;
    const double length = std::hypot(std::hypot(q[0], q[1]),
                                     std::hypot(q[2], q[3]));
    if (!std::isfinite(length) || length <= 0.0) return false;
    const double w = q[0] / length, x = q[1] / length;
    const double y = q[2] / length, z = q[3] / length;
    r = {{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
          {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
          {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
    return true;
  }
  static bool quaternionFromRotation(const Matrix &r, std::array<double, 4> &q) {
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        if (!std::isfinite(r[i][j]) ||
            std::abs(dot(column(r, i), column(r, j)) - (i == j ? 1.0 : 0.0)) > 1e-10)
          return false;
    if (std::abs(determinant(r) - 1.0) > 1e-10) return false;
    const double trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0) {
      const double s = 2.0 * std::sqrt(trace + 1.0);
      q = {0.25 * s, (r[2][1] - r[1][2]) / s,
           (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s};
    } else {
      int i = 0;
      if (r[1][1] > r[i][i]) i = 1;
      if (r[2][2] > r[i][i]) i = 2;
      const int j = (i + 1) % 3, k = (j + 1) % 3;
      const double s = 2.0 * std::sqrt(1.0 + r[i][i] - r[j][j] - r[k][k]);
      if (!std::isfinite(s) || s <= 0.0) return false;
      q[0] = (r[k][j] - r[j][k]) / s;
      q[i + 1] = 0.25 * s;
      q[j + 1] = (r[j][i] + r[i][j]) / s;
      q[k + 1] = (r[k][i] + r[i][k]) / s;
    }
    const double length = std::hypot(std::hypot(q[0], q[1]),
                                     std::hypot(q[2], q[3]));
    if (!std::isfinite(length) || length <= 0.0) return false;
    const double sign = q[0] < 0.0 ? -1.0 : 1.0;
    for (double &value : q) value = value * sign / length;
    return true;
  }

  static std::array<double, 25> sphericalHarmonics(const Vector &direction) {
    std::array<double, 25> result{};
    const double z = std::clamp(direction[2], -1.0, 1.0);
    const double phi = std::atan2(direction[1], direction[0]);
    const double radial = std::sqrt(std::max(0.0, 1.0 - z * z));
    for (int m = 0; m <= 4; ++m) {
      double pmm = 1.0;
      for (int k = 1; k <= m; ++k) pmm *= -(2 * k - 1) * radial;
      double previousPrevious = pmm, previous = pmm;
      for (int l = m; l <= 4; ++l) {
        double polynomial = pmm;
        if (l == m + 1) polynomial = z * (2 * m + 1) * pmm;
        else if (l > m + 1)
          polynomial = ((2 * l - 1) * z * previous -
                        (l + m - 1) * previousPrevious) / (l - m);
        double factorialRatio = 1.0;
        for (int k = l - m + 1; k <= l + m; ++k) factorialRatio /= k;
        const double normalization = std::sqrt((2 * l + 1) * factorialRatio / (4 * Pi));
        if (m == 0) result[l * (l + 1)] = normalization * polynomial;
        else {
          const double value = std::sqrt(2.0) * normalization * polynomial;
          result[l * (l + 1) + m] = value * std::cos(m * phi);
          result[l * (l + 1) - m] = value * std::sin(m * phi);
        }
        previousPrevious = previous;
        previous = polynomial;
      }
    }
    return result;
  }

  void prepareShTransform() {
    // Products of two degree<=4 SH are sphere polynomials of degree<=8.
    // Nine equispaced azimuths integrate all Fourier modes |m|<=8 exactly;
    // the surviving z polynomial is integrated exactly by five GL nodes.
    // Thus this is an exact band transform in real arithmetic, not a sampled
    // fit/Fibonacci approximation. Only double-precision rounding remains.
    constexpr std::array<double, 5> nodes{
        0.0, -0.53846931010568309104, 0.53846931010568309104,
        -0.90617984593866399280, 0.90617984593866399280};
    constexpr std::array<double, 5> weights{
        0.56888888888888888889, 0.47862867049936646804,
        0.47862867049936646804, 0.23692688505618908751,
        0.23692688505618908751};
    for (int z = 0; z < 5; ++z) {
      const double radius = std::sqrt(1.0 - nodes[z] * nodes[z]);
      for (int azimuth = 0; azimuth < 9; ++azimuth) {
        const double phi = 2.0 * Pi * azimuth / 9.0;
        const Vector direction{radius * std::cos(phi), radius * std::sin(phi), nodes[z]};
        // Orthogonal Q can be proper or improper: Q^-1=Q^T. Direct basis
        // evaluation automatically gives odd-band parity for reflections.
        Vector local{};
        for (int row = 0; row < 3; ++row)
          for (int col = 0; col < 3; ++col)
            local[row] += mQ[col][row] * direction[col];
        const auto outputBasis = sphericalHarmonics(direction);
        const auto inputBasis = sphericalHarmonics(local);
        const double weight = weights[z] * 2.0 * Pi / 9.0;
        for (int l = 1; l <= 4; ++l)
          for (int j = l * l; j < (l + 1) * (l + 1); ++j)
            for (int i = l * l; i < (l + 1) * (l + 1); ++i)
              mSh[j][i] += weight * outputBasis[j] * inputBasis[i];
      }
    }
  }
};

} // namespace gsw::detail
