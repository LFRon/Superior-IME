// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "superior/api/Composer.hpp"
#include "superior/api/Composition.hpp"
#include "superior/pinyin/CandidatePipeline.hpp"
#include "superior/pinyin/CloudService.hpp"
#include "superior/pinyin/LearningStore.hpp"
#include "superior/pinyin/LibrimePort.hpp"
#include "superior/pinyin/RimeEngine.hpp"

namespace superior::pinyin {

/// The pinyin capability: librime + the candidate pipeline.
///
/// This is the only place that ties the engine, the user's learning and the
/// cloud answer together. It has no host dependency: the runtime gives it
/// neutral keys and options, and reads `CompositionFact` back.
class RimeComposer final : public api::Composer {
public:
    RimeComposer(std::unique_ptr<RimePort> port, const Paths &paths, LearningStore &learning,
                 CloudService &cloud, const api::PipelineOptions &options, std::string &error);
    ~RimeComposer() override;

    RimeComposer(const RimeComposer &) = delete;
    RimeComposer &operator=(const RimeComposer &) = delete;

    [[nodiscard]] bool available() const override;

    api::KeyOutcome processKey(const api::KeyPress &key, std::uint64_t nowMs) override;
    api::KeyOutcome select(std::size_t row, std::uint64_t nowMs) override;
    bool moveSelection(int delta) override;
    bool page(int delta) override;

    bool commitLetters(std::string &out) override;
    void reset() override;

    void setOptions(const api::PipelineOptions &options) override;
    void setOption(std::string_view name, bool value) override;
    [[nodiscard]] bool getOption(std::string_view name) override;

    [[nodiscard]] api::CompositionFact frame() const override;
    void tick(std::uint64_t nowMs) override;

private:
    void refresh();

    std::unique_ptr<RimePort> port_;
    RimeEngine engine_{*port_};
    LearningStore &learning_;
    CloudService &cloud_;
    api::PipelineOptions options_;
    CandidatePipeline pipeline_;

    bool started_ = false;
    std::string pinyin_;
    std::string cloudText_;
    std::string queried_;
    std::uint64_t lastKeyMs_ = 0;
};

/// Creates one `RimeComposer` per input context.
class RimeComposerFactory final : public api::ComposerFactory {
public:
    RimeComposerFactory(Paths paths, LearningStore &learning, CloudService &cloud);

    [[nodiscard]] bool available() const override;
    [[nodiscard]] std::string unavailableReason() const override;

    std::unique_ptr<api::Composer> create(std::string &error) override;

private:
    Paths paths_;
    LearningStore &learning_;
    CloudService &cloud_;
    bool available_ = false;
    std::string unavailable_;
};

} // namespace superior::pinyin
