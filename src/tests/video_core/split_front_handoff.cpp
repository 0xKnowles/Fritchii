// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// GPU thread split ownership transfer (split_front_ownership.h). These drive the real Maxwell3D
// and KeplerCompute front/own method paths with the split stopped (GpuSplit::Drain is a no-op
// then, and the front paths used here only collect macro parameters, so no ring is needed).
// They check engine state across split -> unsplit -> split, not frame rate or semaphore progress
// in a game.

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/split_front_ownership.h"
#include "video_core/gpu_split.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"

namespace {

using Tegra::Engines::KeplerCompute;
using Tegra::Engines::Maxwell3D;
using Tegra::Engines::SplitFrontOwnership;

class StubOwner final : public Tegra::Engines::EngineInterface {
public:
    ~StubOwner() override { SplitFrontOwnership::Unregister(this); }
    void CallMethod(Core::System&, u32, u32, bool) override {}
    void CallMultiMethod(Core::System&, u32, const u32*, u32, u32) override {}
    void ReturnSplitFrontOwnership() override {
        ++returns;
        SplitFrontOwnership::Unregister(this);
    }
    int returns{};
};

struct EngineFixture {
    Core::System system;
    Core::DeviceMemory device_memory;
    Tegra::MaxwellDeviceMemoryManager device_manager{device_memory};
    Tegra::MemoryManager memory{system, device_manager, 32, 1ULL << 31, 16, 12};
    std::shared_ptr<Tegra::NativeMacroContext> macros = [] {
        auto context = std::make_shared<Tegra::NativeMacroContext>();
        context->engine.native_turing = true;
        context->compute_engine.native_turing = true;
        return context;
    }();

    void NativeChannel(Maxwell3D& maxwell3d, KeplerCompute& compute) {
        // As ChannelState::Init for a Switch 2 channel.
        maxwell3d.SetNativeMacroContext(macros);
        maxwell3d.SetNativeTuringMacroMode(true);
        compute.SetNativeMacroMode(&macros->engine, macros->positions, macros.get());
        compute.SetNativeDataRam(&maxwell3d.GetNativeDataRam());
    }
};

VideoCommon::GpuSplit& StoppedSplit() {
    auto& split = VideoCommon::GpuSplit::Get();
    REQUIRE_FALSE(split.Active());
    return split;
}

constexpr u32 MacroMethod = 0xE00;
constexpr u32 PlainRegister = MAXWELL3D_REG_INDEX(shadow_scratch) + 1;

} // Anonymous namespace

TEST_CASE("Split front ownership registry hands each engine back once",
          "[video_core][split_handoff]") {
    REQUIRE(SplitFrontOwnership::Count() == 0);
    StubOwner a;
    StubOwner b;
    for (int cycle = 1; cycle <= 3; ++cycle) {
        SplitFrontOwnership::Register(&a);
        SplitFrontOwnership::Register(&a); // Idempotent
        SplitFrontOwnership::Register(&b);
        REQUIRE(SplitFrontOwnership::Count() == 2);
        REQUIRE(SplitFrontOwnership::ReturnAll() == 2);
        REQUIRE(a.returns == cycle);
        REQUIRE(b.returns == cycle);
        REQUIRE(SplitFrontOwnership::Count() == 0);
    }
    REQUIRE(SplitFrontOwnership::ReturnAll() == 0);
    {
        StubOwner destroyed;
        SplitFrontOwnership::Register(&destroyed);
        SplitFrontOwnership::Unregister(&destroyed);
    }
    REQUIRE(SplitFrontOwnership::ReturnAll() == 0);
}

TEST_CASE("Split takeover consumes held state writes with each shadow RAM mode",
          "[video_core][split_handoff]") {
    EngineFixture f;
    auto& split = StoppedSplit();
    using Mode = Maxwell3D::Regs::ShadowRamControl;
    for (const auto mode : {Mode::Track, Mode::TrackWithFilter, Mode::Replay, Mode::Passthrough}) {
        Maxwell3D engine{f.memory};
        engine.CallMethod(f.system, PlainRegister, 7, true);
        engine.CallMethod(f.system, MAXWELL3D_REG_INDEX(shadow_ram_control),
                          static_cast<u32>(mode), true);
        engine.method_sink.emplace_back(PlainRegister, 18);
        engine.method_sink.emplace_back(PlainRegister, 19);
        engine.TakeSplitFrontOwnership(f.system, split);
        const u32 expected_register = mode == Mode::Replay ? 7 : 19;
        const u32 expected_shadow = mode == Mode::Track || mode == Mode::TrackWithFilter ? 19 : 7;
        REQUIRE(engine.method_sink.empty());
        REQUIRE(engine.regs.reg_array[PlainRegister] == expected_register);
        REQUIRE(engine.GetSplitFrontRegister(PlainRegister) == expected_register);
        REQUIRE(engine.GetSplitFrontShadow(PlainRegister) == expected_shadow);
        REQUIRE(SplitFrontOwnership::ReturnAll() == 1);
    }
}

TEST_CASE("Maxwell3D macro uploads and binds apply unsplit after the split hands back",
          "[video_core][split_handoff]") {
    REQUIRE(SplitFrontOwnership::Count() == 0);
    EngineFixture f;
    Maxwell3D maxwell3d{f.memory};
    KeplerCompute compute{f.memory};
    f.NativeChannel(maxwell3d, compute);
    auto& split = StoppedSplit();

    maxwell3d.TakeSplitFrontOwnership(f.system, split);
    REQUIRE(maxwell3d.GetSplitHandoffState().front_owned);
    REQUIRE(SplitFrontOwnership::Count() == 1);
    REQUIRE(SplitFrontOwnership::ReturnAll() == 1);
    REQUIRE_FALSE(maxwell3d.GetSplitHandoffState().front_owned);
    REQUIRE_FALSE(maxwell3d.GetSplitFrontRegister(PlainRegister).has_value());

    // Before the handback existed these were skipped forever once the front had owned macros.
    const u64 generation = f.macros->engine.native_code_generation;
    maxwell3d.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), 2, true);
    maxwell3d.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.instruction), 0xabc, true);
    maxwell3d.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.instruction), 0xdef, true);
    REQUIRE(f.macros->engine.native_code[6] == 0xabc);
    REQUIRE(f.macros->engine.native_code[7] == 0xdef);
    REQUIRE(f.macros->engine.native_code_generation == generation + 2);

    maxwell3d.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), 5, true);
    maxwell3d.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.start_address), 0x30, true);
    maxwell3d.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.start_address), 0x31, true);
    REQUIRE(f.macros->positions[5] == 0x30);
    REQUIRE(f.macros->positions[6] == 0x31);
    REQUIRE(maxwell3d.regs.load_mme.start_address_ptr == 7);

    // The next split start mirrors the pointer the unsplit binds advanced.
    maxwell3d.TakeSplitFrontOwnership(f.system, split);
    REQUIRE(maxwell3d.GetSplitFrontRegister(MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)) ==
            7U);
    REQUIRE(SplitFrontOwnership::ReturnAll() == 1);
}

TEST_CASE("Maxwell3D front mirror refreshes after unsplit writes and consumes the sink in order",
          "[video_core][split_handoff]") {
    REQUIRE(SplitFrontOwnership::Count() == 0);
    EngineFixture f;
    Maxwell3D maxwell3d{f.memory};
    KeplerCompute compute{f.memory};
    f.NativeChannel(maxwell3d, compute);
    auto& split = StoppedSplit();

    maxwell3d.TakeSplitFrontOwnership(f.system, split);
    REQUIRE(maxwell3d.GetSplitFrontRegister(PlainRegister) == 0U);
    SplitFrontOwnership::ReturnAll();

    // An unsplit write the restarted front must see: a stale mirror holding 0 would make a
    // later front write of 0 look like a same-value write and skip it.
    maxwell3d.CallMethod(f.system, PlainRegister, 7, true);
    REQUIRE(maxwell3d.regs.reg_array[PlainRegister] == 7);
    maxwell3d.TakeSplitFrontOwnership(f.system, split);
    REQUIRE(maxwell3d.GetSplitFrontRegister(PlainRegister) == 7U);
    REQUIRE(maxwell3d.GetSplitFrontShadow(PlainRegister) == 7U);
    SplitFrontOwnership::ReturnAll();

    // Held writes actually complete before the mirror is taken. In live split mode
    // they execute on the back, including any deferred draw they trigger.
    maxwell3d.method_sink.emplace_back(PlainRegister, 9);
    maxwell3d.TakeSplitFrontOwnership(f.system, split);
    REQUIRE(maxwell3d.GetSplitFrontRegister(PlainRegister) == 9U);
    REQUIRE(maxwell3d.GetSplitFrontShadow(PlainRegister) == 9U);
    REQUIRE(maxwell3d.method_sink.empty());
    REQUIRE(maxwell3d.regs.reg_array[PlainRegister] == 9);
    SplitFrontOwnership::ReturnAll();
    maxwell3d.ConsumeSink(f.system);
    REQUIRE(maxwell3d.regs.reg_array[PlainRegister] == 9);

    // Replay: the held write's register gets the shadow value, on both sides.
    maxwell3d.CallMethod(f.system, MAXWELL3D_REG_INDEX(shadow_ram_control),
                         static_cast<u32>(Maxwell3D::Regs::ShadowRamControl::Replay), true);
    maxwell3d.method_sink.emplace_back(PlainRegister, 42);
    maxwell3d.TakeSplitFrontOwnership(f.system, split);
    REQUIRE(maxwell3d.GetSplitFrontRegister(PlainRegister) == 9U);
    SplitFrontOwnership::ReturnAll();
    maxwell3d.ConsumeSink(f.system);
    REQUIRE(maxwell3d.regs.reg_array[PlainRegister] == 9);
}

TEST_CASE("Maxwell3D partial macro call survives repeated split transitions",
          "[video_core][split_handoff]") {
    REQUIRE(SplitFrontOwnership::Count() == 0);
    EngineFixture f;
    Maxwell3D maxwell3d{f.memory};
    KeplerCompute compute{f.memory};
    f.NativeChannel(maxwell3d, compute);
    auto& split = StoppedSplit();

    // The call starts unsplit, in one GP entry...
    maxwell3d.current_dma_segment = 0x1000;
    maxwell3d.CallMethod(f.system, MacroMethod, 10, false);
    auto state = maxwell3d.GetSplitHandoffState();
    REQUIRE(state.executing_macro == MacroMethod);
    REQUIRE(state.parameters == 1);
    REQUIRE(state.segment_words == 1);

    size_t expected = 1;
    for (int cycle = 0; cycle < 4; ++cycle) {
        // ...continues on the front after a split start (in single and multi-word runs)...
        maxwell3d.FrontCallMethod(f.system, split, MacroMethod + 1, 11, false);
        const std::array<u32, 2> words{12, 13};
        maxwell3d.FrontCallMultiMethod(f.system, split, MacroMethod + 1, words.data(), 2, 8);
        expected += 3;
        state = maxwell3d.GetSplitHandoffState();
        REQUIRE(state.front_owned);
        REQUIRE(state.front_executing_macro == MacroMethod);
        REQUIRE(state.front_parameters == expected);
        REQUIRE(state.executing_macro == 0);
        REQUIRE(state.parameters == 0);
        REQUIRE(state.segment_words == 0);

        // ...and unsplit again after the split stops: nothing lost, nothing repeated.
        REQUIRE(SplitFrontOwnership::ReturnAll() == 1);
        state = maxwell3d.GetSplitHandoffState();
        REQUIRE_FALSE(state.front_owned);
        REQUIRE(state.executing_macro == MacroMethod);
        REQUIRE(state.parameters == expected);
        REQUIRE(state.segment_words == expected);
        maxwell3d.CallMethod(f.system, MacroMethod + 1, 14, false);
        ++expected;
        state = maxwell3d.GetSplitHandoffState();
        REQUIRE(state.parameters == expected);
        REQUIRE(state.segment_words == expected);
    }
}

TEST_CASE("KeplerCompute macro selection, bindings and partial call transfer",
          "[video_core][split_handoff]") {
    REQUIRE(SplitFrontOwnership::Count() == 0);
    EngineFixture f;
    Maxwell3D maxwell3d{f.memory};
    KeplerCompute compute{f.memory};
    f.NativeChannel(maxwell3d, compute);
    auto& split = StoppedSplit();

    compute.TakeSplitFrontOwnership(split);
    REQUIRE(compute.GetSplitHandoffState().front_owned);
    REQUIRE_FALSE(compute.GetSplitHandoffState().front_compute_uploaded);
    REQUIRE(SplitFrontOwnership::ReturnAll() == 1);

    // Compute macro code and bindings (methods 0x45-0x48) go to the compute engine unsplit.
    compute.CallMethod(f.system, 0x45, 1, true);
    compute.CallMethod(f.system, 0x46, 0x77, true);
    compute.CallMethod(f.system, 0x47, 4, true);
    compute.CallMethod(f.system, 0x48, 0x99, true);
    REQUIRE(f.macros->compute_engine.native_code[3] == 0x77);
    REQUIRE(f.macros->engine.native_code[3] == 0);
    REQUIRE(f.macros->compute_positions[4] == 0x99);
    REQUIRE(f.macros->positions[4] == 0);
    auto state = compute.GetSplitHandoffState();
    REQUIRE(state.compute_uploaded);
    REQUIRE(state.bind_pointer == 5);

    // An unsplit QMD field write the restarted front's same-value skip must compare against.
    compute.CallMethod(f.system, 0xc8, 0x1234, true);

    compute.TakeSplitFrontOwnership(split);
    state = compute.GetSplitHandoffState();
    REQUIRE(state.front_compute_uploaded);
    REQUIRE(state.front_bind_pointer == 5);
    REQUIRE(compute.GetSplitFrontRegister(0x48) == 0x99U);
    REQUIRE(compute.GetSplitFrontRegister(0xc8) == 0x1234U);

    // A compute macro call split across GP entries and split modes.
    compute.FrontCallMethod(f.system, split, MacroMethod, 5, false, nullptr);
    REQUIRE(compute.GetSplitHandoffState().front_parameters == 1);
    REQUIRE(SplitFrontOwnership::ReturnAll() == 1);
    state = compute.GetSplitHandoffState();
    REQUIRE_FALSE(state.front_owned);
    REQUIRE(state.executing_macro == MacroMethod);
    REQUIRE(state.parameters == 1);
    REQUIRE(state.compute_uploaded);
    REQUIRE(state.bind_pointer == 5);

    compute.CallMethod(f.system, MacroMethod + 1, 6, false);
    REQUIRE(compute.GetSplitHandoffState().parameters == 2);
    compute.FrontCallMethod(f.system, split, MacroMethod + 1, 7, false, nullptr);
    state = compute.GetSplitHandoffState();
    REQUIRE(state.front_executing_macro == MacroMethod);
    REQUIRE(state.front_parameters == 3);
    REQUIRE(state.parameters == 0);
    REQUIRE(SplitFrontOwnership::ReturnAll() == 1);
    state = compute.GetSplitHandoffState();
    REQUIRE(state.executing_macro == MacroMethod);
    REQUIRE(state.parameters == 3);
}

TEST_CASE("Split handback covers every channel engine and channel teardown",
          "[video_core][split_handoff]") {
    REQUIRE(SplitFrontOwnership::Count() == 0);
    EngineFixture f;
    auto& split = StoppedSplit();
    Maxwell3D maxwell3d_a{f.memory};
    KeplerCompute compute_a{f.memory};
    Maxwell3D maxwell3d_b{f.memory};
    KeplerCompute compute_b{f.memory};
    // Both channels share the process's macro context, as on the console.
    f.NativeChannel(maxwell3d_a, compute_a);
    f.NativeChannel(maxwell3d_b, compute_b);

    for (int cycle = 0; cycle < 3; ++cycle) {
        maxwell3d_a.TakeSplitFrontOwnership(f.system, split);
        compute_a.TakeSplitFrontOwnership(split);
        maxwell3d_b.TakeSplitFrontOwnership(f.system, split);
        compute_b.TakeSplitFrontOwnership(split);
        maxwell3d_a.TakeSplitFrontOwnership(f.system, split); // Already owned: no second entry
        REQUIRE(SplitFrontOwnership::Count() == 4);
        REQUIRE(SplitFrontOwnership::ReturnAll() == 4);
        REQUIRE_FALSE(maxwell3d_a.GetSplitHandoffState().front_owned);
        REQUIRE_FALSE(compute_a.GetSplitHandoffState().front_owned);
        REQUIRE_FALSE(maxwell3d_b.GetSplitHandoffState().front_owned);
        REQUIRE_FALSE(compute_b.GetSplitHandoffState().front_owned);
    }

    {
        Maxwell3D torn_down{f.memory};
        KeplerCompute torn_down_compute{f.memory};
        f.NativeChannel(torn_down, torn_down_compute);
        torn_down.TakeSplitFrontOwnership(f.system, split);
        torn_down_compute.TakeSplitFrontOwnership(split);
        REQUIRE(SplitFrontOwnership::Count() == 2);
    }
    REQUIRE(SplitFrontOwnership::Count() == 0);
    REQUIRE(SplitFrontOwnership::ReturnAll() == 0);
}
