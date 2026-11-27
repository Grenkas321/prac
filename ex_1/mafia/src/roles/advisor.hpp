// Советник игрока: внешний «мозг», который может предложить решение днём
// (речь, голос, последнее слово). Например, ИИ-агент (ai/ai_advisor.hpp).
//
// Игрок спрашивает советника первым; если ответа нет (сбой, лимит, неверный
// ответ) или ответ недопустим — игрок решает сам, обычной эвристикой.
// Роли о советнике не знают: подключается он к базе Player.
#pragma once

#include "game/protocol.hpp"

#include <optional>
#include <string>

namespace mafia {

struct AdvisorAnswer {
    std::optional<PlayerId> target;  // голос / кого обвиняем или защищаем
    std::string speech;              // реплика
    std::optional<Claim> claim;      // утверждение речи
};

class Advisor {
public:
    virtual ~Advisor() = default;
    // Пусто — совета нет, игрок решает сам. Вызывается из нити игрока.
    virtual std::optional<AdvisorAnswer> advise(const Request& request) = 0;
};

}  // namespace mafia
