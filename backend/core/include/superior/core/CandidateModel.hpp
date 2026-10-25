// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "superior/api/Content.hpp"

namespace superior::core {

/// Ordered candidate list with **generation-stable** ids, paging and selection.
///
/// The ids are assigned here, not by the caller, and each id encodes the list
/// generation together with the position:
///
/// ```text
///   id = (generation << 16) | (position + 1)
/// ```
///
/// That encoding is what makes a stale click harmless. Before, ids were the
/// page-local index (`index + 1`), so a click delivered after the list was
/// rebuilt could select a different word; worse, page 2 restarted at 1 and
/// aliased page 1. With a generation in the id, an id from an older list is
/// rejected instead of silently reinterpreted.
///
/// Other invariants:
/// - a rebuild keeps the highlighted *position* when it still exists, otherwise
///   selects the first candidate;
/// - paging moves the selection to the first item of the new page;
/// - selection is identified by id, never by index, so callers cannot confuse
///   the two.
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

    /// Generation of the current list. Bumped only when the content changes, so
    /// a repaint that produces the same words keeps earlier ids valid.
    [[nodiscard]] std::uint32_t generation() const noexcept;

    /// Id of the candidate at `position` in the current generation, or 0 when
    /// the position is out of range.
    [[nodiscard]] api::CandidateId idAt(std::size_t position) const noexcept;

    /// Position of `id` in the current list, or `std::nullopt` when the id was
    /// issued by an older generation or lies outside the list.
    [[nodiscard]] std::optional<std::size_t> positionOf(api::CandidateId id) const noexcept;

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
    void revealIndex(int index);

    std::vector<api::Candidate> items_;
    int pageSize_ = 7;
    int pageIndex_ = 0;
    api::CandidateId selected_ = 0;
    std::uint32_t generation_ = 1;
};

} // namespace superior::core
