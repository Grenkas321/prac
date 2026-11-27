// Мафиози: ночью голосует за общую жертву мафии. Союзников узнаёт от
// ведущего через learn() и никогда не выбирает их — ни ночью, ни днём,
// ни в обвинениях.
//
// Логика отличается от мирной: мирный ищет мафию по своим оценкам, а мафиози
// смотрит глазами города (town()): днём подставляет того, кого город и так
// подозревает, — голос «со всеми» не выделяет его из толпы. Ночью убирает
// самого опасного для мафии, но не того, чья смерть укажет на союзника.
#pragma once

#include "roles/player.hpp"

#include <array>
#include <set>

namespace mafia {

class Mafioso : public Player {
public:
    static constexpr RoleKind kKind = RoleKind::Mafioso;
    static constexpr std::array<NightActionKind, 1> kNightActions = {NightActionKind::Kill};
    static constexpr bool kMaySkipNight = false;

    explicit Mafioso(PlayerSetup setup) : Player(std::move(setup), kKind) {}

    Response act(const Request& request);
    void learn(const KnownIdentity& fact);

    const std::set<PlayerId>& allies() const noexcept { return allies_; }

protected:
    // Для специализированных мафиози и тех, кто станет мафией позже (подкидыш, оборотень).
    Mafioso(PlayerSetup setup, RoleKind role) : Player(std::move(setup), role) {}

    // Сейчас ли этот игрок в мафии (подкидыш до превращения — мирный).
    static bool in_mafia(const Request& request) { return team_of(request.you.role) == Team::Mafia; }

    int target_score(const Request& request, Purpose purpose, PlayerId candidate) const override;
    std::size_t choose_option(const Request& request) override;
    Statement statement(const Request& request) override;

private:
    bool ally(PlayerId id) const { return allies_.contains(id); }
    // Последнее публичное обвинение или голос candidate был против союзника:
    // если он ночью погибнет, город спросит «кому он мешал?» — и посмотрит на союзника.
    bool exposes_ally(const Request& request, PlayerId candidate) const;

    std::set<PlayerId> allies_;
};

}  // namespace mafia
