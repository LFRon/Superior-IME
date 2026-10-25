// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/CandidateModel.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>

#include "superior/diagnostics/Log.hpp"

namespace superior::core {

namespace diag = ::superior::diagnostics;

namespace {
constexpr int kMinPageSize = 1;
constexpr int kMaxPageSize = 32;
} // namespace

void CandidateModel::set(std::vector<api::Candidate> items, int pageSize) {
    pageSize_ = std::clamp(pageSize, kMinPageSize, kMaxPageSize);

    std::vector<api::Candidate> accepted;
    accepted.reserve(items.size());
    std::set<api::CandidateId> seen;
    for (auto &candidate : items) {
        if (candidate.id == 0) {
            continue;
        }
        if (!seen.insert(candidate.id).second) {
            continue;
        }
        accepted.push_back(std::move(candidate));
    }
    items_ = std::move(accepted);

    if (items_.empty()) {
        SUPERIOR_TRACE(diag::categories::Core) << "candidate list cleared";
        selected_ = 0;
        pageIndex_ = 0;
        return;
    }

    const int selectedIndex = indexOf(selected_);
    if (selectedIndex < 0) {
        selected_ = items_.front().id;
        pageIndex_ = 0;
    } else {
        pageIndex_ = selectedIndex / pageSize_;
    }

    SUPERIOR_DEBUG(diag::categories::Core)
        .field("count", static_cast<std::uint64_t>(items_.size()))
        .field("pageSize", static_cast<std::int64_t>(pageSize_))
        .field("pages", static_cast<std::int64_t>(pageCount()))
        .field("selected", static_cast<std::uint64_t>(selected_))
        << "candidate list updated";
}

void CandidateModel::clear() {
    items_.clear();
    selected_ = 0;
    pageIndex_ = 0;
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
    const int index = indexOf(id);
    if (index < 0) {
        return false;
    }
    revealIndex(index);
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
    const int index = indexOf(selected_);
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
    const int index = indexOf(selected_);
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
    return indexOf(selected_);
}

const api::Candidate *CandidateModel::selected() const {
    return find(selected_);
}

const api::Candidate *CandidateModel::find(api::CandidateId id) const {
    if (id == 0) {
        return nullptr;
    }
    const int index = indexOf(id);
    if (index < 0) {
        return nullptr;
    }
    return &items_[static_cast<std::size_t>(index)];
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

int CandidateModel::indexOf(api::CandidateId id) const {
    if (id == 0) {
        return -1;
    }
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void CandidateModel::revealIndex(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    selected_ = items_[static_cast<std::size_t>(index)].id;
    pageIndex_ = index / pageSize_;
}

} // namespace superior::core
