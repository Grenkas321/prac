// Комиссар: мирный, каждую ночь либо проверяет игрока, либо стреляет.
// Личная память — результаты своих проверок (доставляет ведущий через learn()).
// Они же попадают в Beliefs как достоверное знание: голос днём — против
// проверенного мафиози, проверка ночью — самого подозрительного из непроверенных.
#pragma once

#include "roles/civilian.hpp"

#include <array>
#include <map>

namespace mafia {

class Commissar : public Civilian {
public:
    static constexpr RoleKind kKind = RoleKind::Commissar;
    static constexpr std::array<NightActionKind, 2> kNightActions = {NightActionKind::Check,
                                                                     NightActionKind::Shoot};
    static constexpr bool kMaySkipNight = false;

    explicit Commissar(PlayerSetup setup) : Civilian(std::move(setup), kKind) {}

    // Ночь: известный живой мафиози среди целей — выстрел; иначе — проверка
    // самого подозрительного из ещё не проверенных.
    Response act(const Request& request);
    void learn(const KnownIdentity& fact);

    // Для тестов и полного лога: что комиссар знает. Только чтение.
    const std::map<PlayerId, bool>& checked() const noexcept { return checked_; }

protected:
    int target_score(const Request& request, Purpose purpose, PlayerId candidate) const override;
    Statement statement(const Request& request) override;
    Statement farewell(const Request& request) override;

private:
    std::map<PlayerId, bool> checked_;  // номер -> мафия ли
};

}  // namespace mafia
