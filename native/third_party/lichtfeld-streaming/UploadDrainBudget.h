/* SPDX-FileCopyrightText: 2026 LichtFeld Studio Authors
 * SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>

namespace gsw::lichtfeld {

// An upload-batch policy, not a GPU-residency or global-frame budget. Call
// permits() before taking the next whole page and recordPage() for every page
// taken, including failed/invalid attempts. Zero byte/time limits are unlimited;
// zero maxPages permits no work. The first page may exceed byte/time limits so
// an indivisible large page cannot permanently starve.
class UploadDrainBudget final {
public:
  explicit UploadDrainBudget(std::uint64_t maxBytes = 32ULL * 1024 * 1024,
                             std::uint32_t maxPages = 8,
                             std::uint64_t maxElapsedNs = 4'000'000) noexcept;

  [[nodiscard]] bool permits(std::uint64_t nextBytes,
                             std::uint64_t elapsedNs) const noexcept;
  void recordPage(std::uint64_t pageBytes) noexcept;

private:
  std::uint64_t mMaxBytes;
  std::uint32_t mMaxPages;
  std::uint64_t mMaxElapsedNs;
  std::uint64_t mConsumedBytes = 0;
  std::uint32_t mConsumedPages = 0;
};

} // namespace gsw::lichtfeld
