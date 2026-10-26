// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/CandidateModel.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "superior/diagnostics/Log.hpp"

namespace superior::core {

namespace diag = ::superior::diagnostics;

namespace {

constexpr int kMinPageSize = 1;
constexpr int kMaxPageSize = 32;
constexpr std::uint32_t kPositionBits = 16;
// Written as a shift rather than a literal so `scripts/check-boundaries.sh` can
// forbid X11 keysym values in `core/` without a false positive here. The point
// of that rule is that no platform numbering may live in the decision layer.
constexpr api::CandidateId kPositionMask = (api::CandidateId{1} << kPositionBits) - 1U;
constexpr std::uint32_t kMaxGeneration = (1U << kPositionBits) - 1U;

} // namespace

void CandidateModel::set(std::vector<api::Candidate> items, int pageSize) {
    pageSize_ = std::clamp(pageSize, kMinPageSize, kMaxPageSize);

    // Remember the highlighted position, not the id: ids belong to the
    // generation that is about to be replaced.
    const int previousIndex = selectedIndex();

    const bool contentChanged =
        items.size() != items_.size() ||
        !std::equal(items.begin(), items.end(), items_.begin(),
                    [](const api::Candidate &left, const api::Candidate &right) {
                        return left.text == right.text && left.comment == right.comment;
                    });
    if (contentChanged && !items.empty()) {
        generation_ = generation_ % kMaxGeneration + 1;
    }

    items_ = std::move(items);
    for (std::size_t index = 0; index < items_.size(); ++index) {
        items_[index].id = idAt(index);
    }

    if (items_.empty()) {
        SUPERIOR_TRACE(diag::categories::Core) << "candidate list cleared";
        selected_ = 0;
        pageIndex_ = 0;
        return;
    }

    if (previousIndex >= 0 && previousIndex < static_cast<int>(items_.size())) {
        revealIndex(previousIndex);
    } else {
        revealIndex(0);
    }

    SUPERIOR_DEBUG(diag::categories::Core)
        .field("count", static_cast<std::uint64_t>(items_.size()))
        .field("pageSize", static_cast<std::int64_t>(pageSize_))
        .field("pages", static_cast<std::int64_t>(pageCount()))
        .field("generation", static_cast<std::uint64_t>(generation_))
        .field("selected", static_cast<std::uint64_t>(selected_))
        << "candidate list updated";
}

void CandidateModel::clear() {
    items_.clear();
    selected_ = 0;
    pageIndex_ = 0;
    // The next list is a new composition: ids from before must not resolve.
    generation_ = generation_ % kMaxGeneration + 1;
}

bool CandidateModel::empty() const noexcept {
    return items_.empty();
}

std::size_t CandidateModel::size() const noexcept {
    return items_.size();
}

int CandidateModel::pageSize() const noexcept {
    return pageSize_;
}

int CandidateModel::pageIndex() const noexcept {
    return pageIndex_;
}

int CandidateModel::pageCount() const noexcept {
    if (items_.empty()) {
        return 0;
    }
    const int count = static_cast<int>(items_.size());
    return (count + pageSize_ - 1) / pageSize_;
}

std::uint32_t CandidateModel::generation() const noexcept {
    return generation_;
}

api::CandidateId CandidateModel::idAt(std::size_t position) const noexcept {
    if (position >= items_.size()) {
        return 0;
    }
    return static_cast<api::CandidateId>((generation_ << kPositionBits) |
                                         static_cast<std::uint32_t>(position + 1));
}

std::optional<std::size_t> CandidateModel::positionOf(api::CandidateId id) const noexcept {
    if (id == 0) {
        return std::nullopt;
    }
    const auto generation = static_cast<std::uint32_t>(id >> kPositionBits);
    if (generation != generation_) {
        return std::nullopt;
    }
    const auto slot = static_cast<std::size_t>(id & kPositionMask);
    if (slot == 0 || slot > items_.size()) {
        return std::nullopt;
    }
    return slot - 1;
}

api::CandidatePage CandidateModel::page() const {
    api::CandidatePage result;
    result.pageIndex = pageIndex_;
    result.pageCount = pageCount();
    result.hasPrevious = pageIndex_ > 0;
    result.hasNext = pageCount() > 0 && pageIndex_ + 1 < pageCount();
    if (items_.empty()) {
        return result;
    }

    const auto begin = static_cast<std::size_t>(pageIndex_) * static_cast<std::size_t>(pageSize_);
    const std::size_t end = std::min(items_.size(), begin + static_cast<std::size_t>(pageSize_));
    result.items.assign(items_.begin() + static_cast<std::ptrdiff_t>(begin),
                        items_.begin() + static_cast<std::ptrdiff_t>(end));
    return result;
}

bool CandidateModel::nextPage() {
    if (pageIndex_ + 1 >= pageCount()) {
        return false;
    }
    revealIndex((pageIndex_ + 1) * pageSize_);
    SUPERIOR_TRACE(diag::categories::Core)
        .field("page", static_cast<std::int64_t>(pageIndex_))
        << "candidate page next";
    return true;
}

bool CandidateModel::previousPage() {
    if (pageIndex_ <= 0) {
        return false;
    }
    revealIndex((pageIndex_ - 1) * pageSize_);
    SUPERIOR_TRACE(diag::categories::Core)
        .field("page", static_cast<std::int64_t>(pageIndex_))
        << "candidate page previous";
    return true;
}

bool CandidateModel::select(api::CandidateId id) {
    const auto position = positionOf(id);
    if (!position) {
        return false;
    }
    revealIndex(static_cast<int>(*position));
    return true;
}

bool CandidateModel::selectIndex(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return false;
    }
    revealIndex(index);
    return true;
}

bool CandidateModel::selectNext() {
    const int index = selectedIndex();
    if (items_.empty()) {
        return false;
    }
    if (index < 0) {
        revealIndex(0);
        return true;
    }
    if (index + 1 >= static_cast<int>(items_.size())) {
        return false;
    }
    revealIndex(index + 1);
    return true;
}

bool CandidateModel::selectPrevious() {
    const int index = selectedIndex();
    if (index <= 0) {
        return false;
    }
    revealIndex(index - 1);
    return true;
}

api::CandidateId CandidateModel::selectedId() const noexcept {
    return selected_;
}

int CandidateModel::selectedIndex() const {
    const auto position = positionOf(selected_);
    return position ? static_cast<int>(*position) : -1;
}

const api::Candidate *CandidateModel::selected() const {
    return find(selected_);
}

const api::Candidate *CandidateModel::find(api::CandidateId id) const {
    const auto position = positionOf(id);
    if (!position) {
        return nullptr;
    }
    return &items_[*position];
}

std::optional<std::string> CandidateModel::takeSelected() {
    const api::Candidate *candidate = selected();
    if (candidate == nullptr) {
        return std::nullopt;
    }
    std::string text = candidate->text;
    clear();
    return text;
}

void CandidateModel::revealIndex(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    selected_ = items_[static_cast<std::size_t>(index)].id;
    pageIndex_ = index / pageSize_;
}

} // namespace superior::core
