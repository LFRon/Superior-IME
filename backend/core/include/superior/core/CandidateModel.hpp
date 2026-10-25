// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "superior/api/Content.hpp"

namespace superior::core {

/// Ordered candidate list with stable ids, paging and id-based selection.
///
/// Semantics:
/// - ids are assigned by the engine and must be non-zero; duplicates are dropped
///   defensively, keeping the first occurrence;
/// - a new list keeps the current selection when its id still exists, otherwise
///   selects the first candidate;
/// - paging moves the selection to the first item of the new page so the
///   highlighted candidate is always visible;
/// - selection is identified by id, never by index, so an in-flight click cannot
///   land on a different candidate after a list update.
class CandidateModel {
public:
    void set(std::vector<api::Candidate> items, int pageSize);
    void clear();

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] int pageSize() const noexcept;
    [[nodiscard]] int pageIndex() const noexcept;
    [[nodiscard]] int pageCount() const noexcept;

    [[nodiscard]] api::CandidatePage page() const;

    bool nextPage();
    bool previousPage();

    bool select(api::CandidateId id);
    bool selectIndex(int index);
    bool selectNext();
    bool selectPrevious();

    [[nodiscard]] api::CandidateId selectedId() const noexcept;
    [[nodiscard]] int selectedIndex() const;
    [[nodiscard]] const api::Candidate *selected() const;
    [[nodiscard]] const api::Candidate *find(api::CandidateId id) const;

    /// Returns the selected candidate text and clears the model.
    std::optional<std::string> takeSelected();

private:
    [[nodiscard]] int indexOf(api::CandidateId id) const;
    void revealIndex(int index);

    std::vector<api::Candidate> items_;
    int pageSize_ = 7;
    int pageIndex_ = 0;
    api::CandidateId selected_ = 0;
};

} // namespace superior::core
