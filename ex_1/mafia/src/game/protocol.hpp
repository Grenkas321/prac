// Протокол «ведущий -> игрок» (Request) и «игрок -> ведущий» (Response).
//
// Это сообщения, а не общие структуры: ведущий собирает Request, отдаёт его
// игроку и больше не трогает; игрок собирает Response и отдаёт ведущему.
// Тайна каждого игрока попадает только в его собственный Request.
#pragma once

#include "core/shared_ptr.hpp"
#include "game/state.hpp"
#include "game/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mafia {

// ---------------------------------------------------------------------------
// Снимок публичного состояния: то, что видит любой сидящий за столом.
// ---------------------------------------------------------------------------

// Игрок глазами остальных: роли нет, есть только объявленное ведущим.
struct PublicPlayer {
    PlayerId id = 0;
    std::string name;
    bool alive = true;
    std::optional<Disclosure> disclosed;
};

struct PublicSnapshot {
    int day = 0;
    Phase phase = Phase::Setup;
    int mafia_total = 0;  // сколько мафиози в партии — ведущий объявляет вслух в начале
    std::vector<PublicPlayer> players;  // индекс == PlayerId
    std::vector<HistoryEntry> history;  // копия публичной истории на момент снимка
};

// ---------------------------------------------------------------------------
// Личная сводка: то, что знает только адресат запроса.
// ---------------------------------------------------------------------------

// KnownIdentity (достоверное знание о другом игроке) объявлен в state.hpp:
// ведущий копит такие сведения в GameState::extra.facts.

struct Briefing {
    PlayerId self = 0;
    RoleKind role = RoleKind::Civilian;  // текущая роль (может смениться: сержант -> комиссар)
    std::vector<KnownIdentity> known;
    std::vector<std::string> notes;      // прочие личные сведения текстом (ушастик, журналист, ...)
};

// ---------------------------------------------------------------------------
// Запрос ведущего.
// ---------------------------------------------------------------------------

enum class RequestKind {
    NightMove,  // ночной ход: actions + targets
    Speech,     // реплика в дневном обсуждении
    Vote,       // голос против одного из targets
    LastWord,   // прощальное слово выбывшего
    Choice,     // выбор одного из options (например «выгнать всех разом?»: да/нет)
    Stop,       // игра окончена: нить игрока должна завершиться
};

// О чём вопрос в запросе Choice.
enum class ChoiceKind {
    KickAll,  // «выгнать всех разом?»: options = {да, нет}, targets = лидеры
    Execute,  // палач при ничьей: options = лидеры + «помиловать», targets = лидеры
    Revive,   // некромант: options = выбывшие + «не сейчас», targets = выбывшие
};

struct Request {
    std::uint64_t id = 0;  // уникален в партии; ответ ссылается на него
    RequestKind kind = RequestKind::Stop;

    // Один снимок на фазу, общий для всех запросов этой фазы. const: никто
    // его не меняет, поэтому его можно читать из любых нитей без блокировок,
    // а SharedPtr держит снимок живым, пока его читает хоть один игрок.
    SharedPtr<const PublicSnapshot> world;

    Briefing you;  // личное: своя роль и достоверно известное о других

    // Уже отфильтрованные ведущим допустимые варианты.
    std::vector<PlayerId> targets;          // Vote, NightMove; Choice — о ком вопрос (лидеры опроса)
    std::vector<NightActionKind> actions;   // NightMove: что можно сделать
    std::vector<std::string> options;       // Choice: подписи вариантов
    ChoiceKind choice = ChoiceKind::KickAll; // Choice: о чём вопрос
    bool can_abstain = false;               // можно ли не выбирать ничего
};

// ---------------------------------------------------------------------------
// Ответ игрока. NightAction объявлен в types.hpp: он же запись тайного журнала.
// ---------------------------------------------------------------------------

struct Response {
    std::uint64_t request_id = 0;  // на какой Request это ответ
    PlayerId from = 0;

    std::optional<PlayerId> target;      // Vote: против кого; пусто — воздержался
    std::string speech;                  // Speech / LastWord / пояснение к голосу или ходу
    std::optional<Claim> claim;          // Speech / LastWord: «X — мафия» или «X — мирный»
    NightAction night;                   // NightMove: какое действие и над кем
    std::optional<std::size_t> choice;   // Choice: индекс в Request::options
};

}  // namespace mafia
