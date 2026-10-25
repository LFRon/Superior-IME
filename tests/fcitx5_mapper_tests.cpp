// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/core/ImeSession.hpp"
#include "superior/fcitx5/PanelMapper.hpp"

namespace {

using superior::api::SessionKey;
using superior::core::ImeSession;
using superior::fcitx5::CandidateEntry;
using superior::fcitx5::PanelFacts;
using superior::fcitx5::PanelMapper;

PanelFacts composingFacts() {
    PanelFacts facts;
    facts.preedit = "nihao";
    facts.preeditCursor = 5;
    for (int i = 1; i <= 9; ++i) {
        CandidateEntry entry;
        entry.id = static_cast<superior::api::CandidateId>(i);
        entry.label = std::to_string(i);
        entry.text = "候选" + std::to_string(i);
        entry.comment = "c" + std::to_string(i);
        facts.candidates.push_back(std::move(entry));
    }
    facts.pageSize = 5;
    facts.pageIndex = 2;
    facts.pageCount = 9;
    facts.cursorIndex = 3;
    facts.cursorRect = superior::api::Rect{100.0, 200.0, 0.0, 20.0};
    facts.cursorValid = true;
    facts.scale = 1.25;
    facts.inputMethod = "pinyin";
    facts.program = "kate";
    facts.statusItems.push_back(superior::api::StatusItem{"ime", "拼", "", true, true});
    facts.composing = true;
    return facts;
}

void testComposingMapping() {
    ImeSession session(SessionKey{1, 1});
    PanelMapper::apply(composingFacts(), session);
    const auto frame = session.snapshot(1);

    CHECK(frame.panel.visible);
    CHECK(frame.panel.preedit.text == "nihao");
    CHECK(frame.panel.preedit.cursor == 5);
    CHECK(frame.panel.candidates.items.size() == 5);
    CHECK(frame.panel.candidates.pageCount == 9);
    CHECK(frame.panel.candidates.pageIndex == 1);
    CHECK(frame.panel.candidates.hasPrevious);
    CHECK(frame.panel.candidates.hasNext);
    CHECK(frame.panel.styleId == "h1");
    CHECK(frame.panel.mode == "pinyin");
    CHECK(frame.panel.skinId == "org.superior.default");
    CHECK(frame.panel.anchor.valid);
    CHECK(superior::test::near(frame.panel.anchor.cursor.x, 100.0));
    CHECK(session.candidates().selectedIndex() == 3);
    CHECK(session.candidates().selectedId() == 4);
    CHECK(frame.status.visible);
    CHECK(frame.status.items.size() == 1);
    CHECK(frame.status.placement == superior::api::StatusPlacement::BottomRight);
}

void testVerticalMapping() {
    ImeSession session(SessionKey{1, 2});
    auto facts = composingFacts();
    facts.vertical = true;
    PanelMapper::apply(facts, session);
    CHECK(session.snapshot(1).panel.styleId == "v1");
}

void testIdleMappingKeepsStatus() {
    ImeSession session(SessionKey{1, 3});
    auto facts = composingFacts();
    PanelMapper::apply(facts, session);
    CHECK(session.visible());

    PanelFacts idle;
    idle.statusItems = facts.statusItems;
    PanelMapper::apply(idle, session);
    const auto frame = session.snapshot(2);
    CHECK(frame.panel.candidates.pageCount == 0);
    CHECK(!frame.panel.visible);
    CHECK(frame.panel.preedit.text.empty());
    CHECK(frame.panel.candidates.items.empty());
    CHECK(frame.status.visible);
}

void testPageSizeAndCursorClamp() {
    ImeSession session(SessionKey{1, 4});
    auto facts = composingFacts();
    facts.pageSize = 0;
    facts.cursorIndex = 999;
    PanelMapper::apply(facts, session);
    CHECK(session.candidates().pageSize() == 1);
    CHECK(session.candidates().selectedIndex() >= 0);

    facts.pageSize = 1000;
    facts.cursorIndex = -1;
    PanelMapper::apply(facts, session);
    CHECK(session.candidates().pageSize() == 32);
}

void testSkinGenerationPropagates() {
    ImeSession session(SessionKey{1, 5});
    PanelMapper::apply(composingFacts(), session, "org.example.custom", 42);
    const auto frame = session.snapshot(1);
    CHECK(frame.panel.skinId == "org.example.custom");
    CHECK(frame.panel.skinGeneration == 42);
    CHECK(frame.status.skinGeneration == 42);
}

} // namespace

int main() {
    testComposingMapping();
    testVerticalMapping();
    testIdleMappingKeepsStatus();
    testPageSizeAndCursorClamp();
    testSkinGenerationPropagates();
    return superior::test::finish("fcitx5 mapper tests");
}
