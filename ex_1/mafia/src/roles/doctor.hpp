// Доктор: мирный, каждую ночь лечит одного игрока.
//
// Правило (вариант 2): не одного и того же две ночи подряд; себя — только
// один раз за игру. Проверяется в двух местах:
//   * may_protect — по тайному журналу GameState::night_log. Им пользуется
//     ведущий, когда собирает для доктора список допустимых целей, и когда
//     проверяет ответ. Это решающая проверка.
//   * act() — по личной памяти доктора (last_healed_, healed_self_).
//     Доктор сам не предлагает запрещённое, даже если список пришёл без фильтра.
#pragma once

#include "roles/civilian.hpp"

#include <array>
#include <optional>

namespace mafia {

class Doctor : public Civilian {
public:
    static constexpr RoleKind kKind = RoleKind::Doctor;
    static constexpr std::array<NightActionKind, 1> kNightActions = {NightActionKind::Heal};
    static constexpr bool kMaySkipNight = false;

    explicit Doctor(PlayerSetup setup) : Civilian(std::move(setup), kKind) {}

    Response act(const Request& request);

    // Можно ли доктору self лечить target в текущую ночь (state.day).
    static bool may_protect(const GameState& state, PlayerId self, PlayerId target);

    std::optional<PlayerId> last_healed() const noexcept { return last_healed_; }
    bool healed_self() const noexcept { return healed_self_; }

protected:
    // Ночью лечит того, кого вероятнее убьют и кто полезнее городу: того, кто
    // сильнее давит на подозреваемых, и самого надёжного; подозрительного — нет.
    int target_score(const Request& request, Purpose purpose, PlayerId candidate) const override;

private:
    std::optional<PlayerId> last_healed_;  // кого лечил прошлой ночью
    bool healed_self_ = false;             // уже лечил себя за эту игру
};

}  // namespace mafia
