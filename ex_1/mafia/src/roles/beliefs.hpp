// Beliefs — что игрок думает о каждом другом игроке.
//
// Два разных вида знания, и они хранятся раздельно:
//   * certain_mafia — ДОСТОВЕРНО: своя роль, союзники мафии, проверки
//     комиссара, публично раскрытые выбывшие. Не меняется от разговоров.
//   * Opinion (suspicion, trust, threat) — ОЦЕНКИ: выводятся из публичных
//     событий, могут ошибаться и меняются с каждым новым событием.
//
// Оценки не копятся по ходу партии: evaluate() каждый раз пересчитывает их
// с нуля по ВСЕЙ публичной истории. Поэтому новое раскрытие (кто-то выбыл и
// оказался мафией) сразу переосмысливает все прошлые события с его участием.
// Это чистая функция: одинаковые входы — одинаковые оценки.
#pragma once

#include "game/protocol.hpp"
#include "game/types.hpp"

#include <optional>
#include <vector>

namespace mafia {

struct Opinion {
    double suspicion = 0;  // насколько похож на мафию (для мирного взгляда)
    double trust = 0;      // насколько можно верить его словам и голосам (0 — обычный)
    double threat = 0;     // насколько он опасен МОЕЙ команде
    double pressure = 0;   // насколько он давит на подозреваемых (обвиняет, голосует) —
                           // такого мафия захочет убрать первым
};

class Beliefs {
public:
    Beliefs() = default;

    // self < 0 — «сторонний наблюдатель»: только публичное знание. Так мафия
    // оценивает, что думает о каждом город (кого выгоднее подставить).
    static Beliefs evaluate(const PublicSnapshot& world, PlayerId self, RoleKind role,
                            const std::vector<KnownIdentity>& known);

    // Достоверно: true — мафия, false — точно не мафия, пусто — не знаю.
    std::optional<bool> certain_mafia(PlayerId id) const;

    // Подозрение с учётом достоверного: известная мафия — очень много,
    // известный не-мафия — очень мало, иначе — оценка.
    double suspicion(PlayerId id) const;
    double trust(PlayerId id) const;
    double threat(PlayerId id) const;
    double pressure(PlayerId id) const;

    const Opinion& opinion(PlayerId id) const;
    std::size_t size() const noexcept { return opinions_.size(); }

    static constexpr double kCertain = 100.0;

private:
    std::vector<Opinion> opinions_;
    std::vector<std::optional<bool>> certain_;
    std::vector<bool> maybe_maniac_;  // проверен как «не мафия», но жив — может быть маньяком
};

}  // namespace mafia
