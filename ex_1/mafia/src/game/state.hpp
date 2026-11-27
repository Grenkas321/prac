// Состояние партии — единственный источник истины об игре.
//
// Кто владеет: Game (через SharedPtr<GameState>), меняет ТОЛЬКО ведущий
// в своей нити. Нити игроков этот объект не видят вообще: им приходит
// неизменяемый снимок PublicSnapshot (см. protocol.hpp), который ведущий
// собирает отсюда, отбрасывая тайное.
#pragma once

#include "game/types.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace mafia {

// Что ведущий публично объявил о выбывшем игроке.
// Закрытые объявления: только is_mafia («мафия / не мафия»).
// Открытые: ещё и точная роль.
struct Disclosure {
    bool is_mafia = false;
    std::optional<RoleKind> role;
};

// Достоверное знание о другом игроке. Мафиози получают так своих союзников
// (is_mafia + роль), проверяющие — результаты проверок, свидетели — убийц.
// is_mafia == true значит «точно враг мирных»: мафия, а если указана роль
// убийцы-одиночки — и он.
struct KnownIdentity {
    PlayerId id = 0;
    bool is_mafia = false;
    std::optional<RoleKind> role;
};

// Состояние одного игрока. Минимум, без которого не обойтись правилам.
struct PlayerState {
    PlayerId id = 0;
    std::string name;
    // ТАЙНА: пока игрок жив, его роль знают только он сам и ведущий
    // (мафиози ещё знают друг друга). В снимок для игроков не попадает.
    RoleKind role = RoleKind::Civilian;
    bool alive = true;
    // Публичная часть: появляется, когда игрок выбывает, и содержит
    // ровно то, что ведущий объявил вслух. Только это поле видят другие.
    std::optional<Disclosure> disclosed;
};

// ---------------------------------------------------------------------------
// Публичная история: только то, что слышали все игроки за столом.
// Ночные ходы, результаты проверок, кто кого лечил, переговоры мафии сюда
// не попадают никогда (их увидит только полный лог).
// ---------------------------------------------------------------------------

enum class DeathCause {
    Executed,  // выбыл по итогам дневного голосования
    Night,     // погиб ночью
    Revenge,   // забран с собой: выстрел Каттани, месть Блейда
};

// Утверждение в речи: «about — мафия» (mafia = true) или «about — мирный».
// Текст речи — для людей, утверждение — для ботов: из него они обновляют оценки.
struct Claim {
    PlayerId about = 0;
    bool mafia = true;
};

struct SpeechEvent {
    PlayerId speaker = 0;
    std::string text;
    std::optional<Claim> claim;
};

struct VoteEvent {
    PlayerId voter = 0;
    std::optional<PlayerId> target;  // пусто — воздержался (если разрешено)
    int round = 1;                   // 2, 3, ... — переголосования среди лидеров
};

// Голос в опросе «выгнать всех разом?» (когда все кандидаты набрали поровну).
struct PollEvent {
    PlayerId voter = 0;
    bool agree = false;
};

struct DeathEvent {
    PlayerId victim = 0;
    DeathCause cause = DeathCause::Night;
    Disclosure disclosed;
    // Только при открытых объявлениях: роль убийцы (не его номер!) —
    // «убит мафией», «убит маньяком», «застрелен комиссаром».
    std::optional<RoleKind> killed_by;
};

struct LastWordEvent {
    PlayerId speaker = 0;
    std::string text;
    std::optional<Claim> claim;  // например, завещание комиссара
};

// Только при открытых объявлениях: «доктор спас игрока N».
struct SavedEvent {
    PlayerId target = 0;
};

// Прочие объявления ведущего: «ночь прошла спокойно», «ничья — переголосование».
struct AnnouncementEvent {
    std::string text;
};

// Публичное раскрытие живого игрока (взлом хакера): всем известен его статус.
struct RevealEvent {
    PlayerId target = 0;
    Disclosure disclosed;
};

// Воскрешение (реаниматор, некромант): игрок снова за столом.
struct ReviveEvent {
    PlayerId target = 0;
};

using Event = std::variant<SpeechEvent, VoteEvent, PollEvent, DeathEvent, LastWordEvent, SavedEvent,
                           AnnouncementEvent, RevealEvent, ReviveEvent>;

struct HistoryEntry {
    int day = 0;
    Phase phase = Phase::Setup;
    Event event;
};

// ---------------------------------------------------------------------------
// Тайный журнал ночей: кто, что и над кем сделал. ТОЛЬКО для ведущего
// (и полного лога); в снимок для игроков не попадает никогда.
// Ограничения ролей выводятся из журнала, а не хранятся отдельными флагами:
// «доктор не лечит одного и того же две ночи подряд» — это поиск его записи
// за ночь N-1. Так же потом проверяются правила доп. ролей
// («не шутит дважды над одним за игру», «не ходит в гости три ночи подряд»).
// ---------------------------------------------------------------------------

struct NightRecord {
    int night = 0;  // ночь N идёт после дня N
    PlayerId actor = 0;
    NightAction action;
};

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Тайное состояние дополнительных ролей — тоже только для ведущего.
// ---------------------------------------------------------------------------

struct Poisoning {
    PlayerId by = 0;
    PlayerId target = 0;
    int night = 0;             // ночь отравления; умирает следующей ночью
    bool neutralized = false;  // доктор лечил в ночь отравления или дано противоядие
};

struct ExtraState {
    std::set<PlayerId> jailed;                      // в тюрьме: не ходят ночью, не голосуют, неуязвимы ночью
    std::optional<PlayerId> silenced;               // шутка громилы: на сегодняшний день
    std::optional<PlayerId> pending_silence;        // шутка громилы: на завтра
    std::optional<PlayerId> frozen;                 // заморозка босса: голос не считается, ночью без хода
    std::optional<PlayerId> pending_freeze;         // заморозка — на следующие сутки
    std::map<PlayerId, std::set<PlayerId>> pranked;   // громила -> над кем уже шутил
    std::map<PlayerId, std::set<PlayerId>> revived;   // реаниматор -> кого уже воскрешал
    std::map<PlayerId, int> honest_kills;           // блейд / отравитель -> сколько мирных убил
    std::vector<Poisoning> poisonings;
    std::set<PlayerId> granted_check;               // дар донора: проверка в следующую ночь
    std::set<PlayerId> fan_done;                    // поклонница уже нашла комиссара
    std::map<PlayerId, int> drunk_streak;           // пьяница -> сколько ночей подряд ходил в гости
    bool necromancer_revealed = false;
    std::vector<PlayerId> pending_revive;           // воскрешённые этой ночью — утром за стол
    // Накопленные личные сведения: что ведущий сообщил игроку (проверки,
    // союзники по особым правилам, свидетельства). Входят в его Briefing.
    std::map<PlayerId, std::vector<KnownIdentity>> facts;
    std::map<PlayerId, std::vector<std::string>> notes;
};

struct GameState {
    int day = 0;  // 0 — до начала; день N и следующая за ним ночь N
    Phase phase = Phase::Setup;
    std::vector<PlayerState> players;   // индекс == PlayerId
    std::vector<HistoryEntry> history;  // только публичные события, по порядку
    std::vector<NightRecord> night_log;  // ТАЙНА: принятые ночные ходы, по порядку
    ExtraState extra;                    // ТАЙНА: состояние дополнительных ролей
    bool finished = false;
    std::optional<Team> winner;  // finished && !winner — ничья (все погибли разом)
};

}  // namespace mafia
