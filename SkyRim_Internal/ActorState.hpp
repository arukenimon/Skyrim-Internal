#pragma once

#include <cstddef>
#include <cstdint>

namespace Skyrim::Offsets::Actor
{
    // Verified in this SkyrimSE.exe database, not portable across updates.
    // Papyrus IsDead (RVA 0x9EA770) dispatches virtual +0x4C8 with dl=1.
    // Actor, Character and PlayerCharacter share RVA 0x677100 in that slot:
    // it masks +0xC8 and accepts exactly these three states when dl=1.
    inline constexpr std::ptrdiff_t kLifeStateFlags = 0xC8;
    inline constexpr std::uint32_t kLifeStateMask = 0x01E0'0000;
    inline constexpr std::uint32_t kDeadStates[] = { 0x0020'0000, 0x0040'0000, 0x00A0'0000 };

    // Papyrus IsPlayerTeammate (RVA 0x9F6FD0) reads bit 26 at +0xE8.
    // Do not infer team membership from lack of hostility or shared race.
    inline constexpr std::ptrdiff_t kActorFlags = 0xE8;
    inline constexpr std::uint32_t kPlayerTeammateFlag = 0x0400'0000;
}

namespace Skyrim::Actors
{
    struct VisualState
    {
        bool dead = false;
        bool playerTeammate = false;

        [[nodiscard]] constexpr bool ShouldDraw() const { return !dead; }
    };

    [[nodiscard]] constexpr VisualState DecodeVisualState(
        const std::uint32_t lifeStateFlags, const std::uint32_t actorFlags)
    {
        const auto lifeState = lifeStateFlags & Offsets::Actor::kLifeStateMask;
        bool dead = false;
        for (const auto deadState : Offsets::Actor::kDeadStates)
            dead = dead || lifeState == deadState;
        return { dead, (actorFlags & Offsets::Actor::kPlayerTeammateFlag) != 0 };
    }

    struct Color { float r, g, b, a; };

    // Other NPCs are not necessarily enemies. Only a true teammate gets
    // green; the player takes precedence even if their teammate bit is set.
    [[nodiscard]] constexpr Color OverlayColor(const VisualState& state, const bool isPlayer)
    {
        if (isPlayer)
            return { 0.35f, 0.60f, 1.0f, 1.0f };
        if (state.playerTeammate)
            return { 0.25f, 1.0f, 0.35f, 1.0f };
        return { 0.2f, 0.95f, 1.0f, 1.0f };
    }
}
