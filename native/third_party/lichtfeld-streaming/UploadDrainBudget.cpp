/* SPDX-FileCopyrightText: 2026 LichtFeld Studio Authors
 * SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "UploadDrainBudget.h"

#include <limits>

namespace gsw::lichtfeld {

UploadDrainBudget::UploadDrainBudget(const std::uint64_t maxBytes,
                                     const std::uint32_t maxPages,
                                     const std::uint64_t maxElapsedNs) noexcept
    : mMaxBytes(maxBytes), mMaxPages(maxPages), mMaxElapsedNs(maxElapsedNs) {}

bool UploadDrainBudget::permits(const std::uint64_t nextBytes,
                                 const std::uint64_t elapsedNs) const noexcept {
  if (mConsumedPages >= mMaxPages) return false;
  // LichtFeld drains at least one whole page even when that page is larger
  // than max_bytes; a zero byte allowance means an unlimited byte drain.
  if (mConsumedPages == 0) return true;
  if (mMaxElapsedNs != 0 && elapsedNs >= mMaxElapsedNs) return false;
  if (mMaxBytes == 0) return true;
  return mConsumedBytes <= mMaxBytes &&
         nextBytes <= mMaxBytes - mConsumedBytes;
}

void UploadDrainBudget::recordPage(const std::uint64_t pageBytes) noexcept {
  constexpr auto maxBytes = std::numeric_limits<std::uint64_t>::max();
  mConsumedBytes = pageBytes > maxBytes - mConsumedBytes
                       ? maxBytes
                       : mConsumedBytes + pageBytes;
  if (mConsumedPages < std::numeric_limits<std::uint32_t>::max()) {
    ++mConsumedPages;
  }
}

} // namespace gsw::lichtfeld
