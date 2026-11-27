// Базовые типы модели игры: идентификаторы, команды, роли, фазы.
// Только данные и чистые функции над ними — поведения игроков здесь нет.
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace mafia {

// Номер места за столом: 0..N-1. Он же индекс в GameState::players.
// Игрокам показываем «Игрок N+1» — это забота вывода, а не модели.
using PlayerId = int;

// Команда определяет условие победы.
enum class Team {
    Civilians,  // мирные: мирный житель, комиссар, доктор и все мирные статусы
    Mafia,      // мафия и специализированные мафиози
    Loner,      // одиночки-убийцы: маньяк, отравитель — играют сами за себя
    Neutral,    // нейтральные: хакер, жулик — не убивают и на победу не влияют
};

// Роль игрока за столом. Ведущего здесь нет: он не сидит за столом.
// Первые пять — обязательные; остальные — дополнительные из конфига.
enum class RoleKind {
    // обязательные
    Civilian,
    Mafioso,
    Commissar,
    Doctor,
    Maniac,
    // статусы мирных жителей
    ReviveCommissar,
    Jailer,
    Priest,
    Journalist,
    Donor,
    Cattani,
    Blade,
    Executioner,
    Ears,
    Prankster,
    // помощники комиссара
    Reanimator,
    Bodyguard,
    Samurai,
    Witness,
    Elder,
    Sergeant,
    Fan,
    Spy,
    Necromancer,
    Drunk,
    // специализированные мафиози (и подкидыш — он мирный)
    Thief,
    Killer,
    Advocate,
    Slanderer,
    Ninja,
    Kamikaze,
    Boss,
    Kidnapper,
    Foundling,
    Bull,
    Paranoid,
    // одиночки
    Poisoner,
    Hacker,
    Swindler,
    Werewolf,
};

// Описание роли: всё неизменное о ней — в одной строке таблицы.
struct RoleInfo {
    RoleKind kind;
    std::string_view id;    // имя в конфиге (латиницей)
    std::string_view name;  // для людей
    Team team;
    std::string_view description;
};

inline constexpr std::array kRoleTable = {
    RoleInfo{RoleKind::Civilian, "civilian", "Мирный житель", Team::Civilians,
             "Ночью спит. Днём ищет мафию и голосует. Побеждает, когда выбыли мафия и одиночки-убийцы."},
    RoleInfo{RoleKind::Mafioso, "mafioso", "Мафиози", Team::Mafia,
             "Знает своих. Ночью мафия сообща выбирает жертву, решение объявляет Босс. "
             "Побеждает, когда мафии не меньше, чем остальных."},
    RoleInfo{RoleKind::Commissar, "commissar", "Комиссар", Team::Civilians,
             "Каждую ночь либо проверяет игрока (мафия или нет), либо стреляет. Пропускать ход нельзя."},
    RoleInfo{RoleKind::Doctor, "doctor", "Доктор", Team::Civilians,
             "Каждую ночь лечит одного: не одного и того же две ночи подряд, себя — один раз за игру."},
    RoleInfo{RoleKind::Maniac, "maniac", "Маньяк", Team::Loner,
             "Одиночка. Каждую ночь убивает любого. Для комиссара выглядит мирным. "
             "Побеждает, оставшись один на один с мирным."},

    RoleInfo{RoleKind::ReviveCommissar, "revive_commissar", "Реанимирующий комиссар", Team::Civilians,
             "Ночью проверяет игрока; проверенного в ту же ночь убить нельзя."},
    RoleInfo{RoleKind::Jailer, "jailer", "Тюремщик", Team::Civilians,
             "Ночью проверяет; если проверенный нечестен (мафия или убийца-одиночка) — сажает его в "
             "тюрьму: заключённый не ходит ночью, не голосует и неуязвим ночью. Смерть тюремщика "
             "освобождает всех."},
    RoleInfo{RoleKind::Priest, "priest", "Священник", Team::Civilians,
             "Ночью исповедует: узнаёт, мафия ли игрок, а сам игрок узнаёт, кто священник."},
    RoleInfo{RoleKind::Journalist, "journalist", "Журналист", Team::Civilians,
             "Ночью сравнивает двух других игроков: одинаковый ли у них статус (мафия / не мафия)."},
    RoleInfo{RoleKind::Donor, "donor", "Донор", Team::Civilians,
             "Ночью проверяет; если проверенный честен — тот следующей ночью сам может кого-то проверить."},
    RoleInfo{RoleKind::Cattani, "cattani", "Комиссар Каттани", Team::Civilians,
             "Проверяет или стреляет. Проверив убитого в ту же ночь, узнаёт убийцу. "
             "Погибая, успевает выстрелить в любого."},
    RoleInfo{RoleKind::Blade, "blade", "Блейд", Team::Civilians,
             "Ночью убивает или пропускает ход; ночью неуязвим. Убив троих мирных, погибает сам. "
             "Казнённый, забирает с собой первого голосовавшего против него."},
    RoleInfo{RoleKind::Executioner, "executioner", "Палач", Team::Civilians,
             "При ничьей в голосовании может казнить одного из лидеров или помиловать всех."},
    RoleInfo{RoleKind::Ears, "ears", "Ушастик", Team::Civilians,
             "Ночью подслушивает у игрока: какие действия были направлены на него (без имён)."},
    RoleInfo{RoleKind::Prankster, "prankster", "Громила", Team::Civilians,
             "Ночью шутит над игроком (над каждым — раз за игру): завтра тот говорит только смайликами, "
             "его голос не считается и его нельзя казнить."},
    RoleInfo{RoleKind::Reanimator, "reanimator", "Реаниматор", Team::Civilians,
             "Ночью может воскресить одного из выбывших (каждого — не больше раза)."},
    RoleInfo{RoleKind::Bodyguard, "bodyguard", "Телохранитель", Team::Civilians,
             "Ночью охраняет игрока: спасает от нападения и убивает нападавшего (от комиссара — просто спасает)."},
    RoleInfo{RoleKind::Samurai, "samurai", "Самурай", Team::Civilians,
             "Ночью охраняет игрока: если на того нападут, самурай гибнет вместо него, убивая нападавшего."},
    RoleInfo{RoleKind::Witness, "witness", "Свидетель", Team::Civilians,
             "Ночью следит за игроком: если на того нападут, узнаёт нападавших."},
    RoleInfo{RoleKind::Elder, "elder", "Старейшина", Team::Civilians,
             "Честный житель, которого нельзя казнить дневным голосованием."},
    RoleInfo{RoleKind::Sergeant, "sergeant", "Сержант", Team::Civilians,
             "Знает комиссара и его проверки, комиссар знает сержанта. Если комиссар погибнет — "
             "сержант становится комиссаром."},
    RoleInfo{RoleKind::Fan, "fan", "Поклонница комиссара", Team::Civilians,
             "Ночью проверяет, комиссар ли игрок. Найдя комиссара, успокаивается."},
    RoleInfo{RoleKind::Spy, "spy", "Шпион", Team::Civilians,
             "Ночью проверяет игрока; результат ведущий передаёт комиссару."},
    RoleInfo{RoleKind::Necromancer, "necromancer", "Некромант", Team::Civilians,
             "Пока жив, выбывшие мирные голосуют половиной голоса. Раз за игру может раскрыться и "
             "воскресить выбывшего — тогда теряет статус. После смерти его статус объявляется."},
    RoleInfo{RoleKind::Drunk, "drunk", "Пьяница", Team::Civilians,
             "Ночью может пойти в гости (не три ночи подряд): хозяин пропускает ход; нападение на "
             "хозяина задевает и гостя, лечение хозяина лечит и гостя; самого пьяницу дома не застать."},

    RoleInfo{RoleKind::Thief, "thief", "Вор", Team::Mafia,
             "Мафиози. Ночью блокирует способность мирного игрока."},
    RoleInfo{RoleKind::Killer, "killer", "Киллер", Team::Mafia,
             "Мафиози. Самостоятельный второй выстрел мафии каждую ночь."},
    RoleInfo{RoleKind::Advocate, "advocate", "Адвокат", Team::Mafia,
             "Мафиози. Ночью узнаёт полный статус игрока (включая маньяка); оставшись единственным "
             "мафиози, получает право стрелять."},
    RoleInfo{RoleKind::Slanderer, "slanderer", "Баламут", Team::Mafia,
             "Мафиози. Ночью оговаривает игрока: при проверке в ту же ночь тот покажется мафией."},
    RoleInfo{RoleKind::Ninja, "ninja", "Ниндзя", Team::Mafia,
             "Мафиози-невидимка: при проверке выглядит мирным."},
    RoleInfo{RoleKind::Kamikaze, "kamikaze", "Камикадзе", Team::Mafia,
             "Мафиози. Ночью ищет комиссара: если нашёл — гибнет вместе с ним."},
    RoleInfo{RoleKind::Boss, "boss", "Босс мафии", Team::Mafia,
             "Решение мафии объявляется через него. Ночью может «заморозить» игрока: завтра его голос "
             "не учитывается, а следующей ночью он пропускает ход. Сам игрок об этом не знает."},
    RoleInfo{RoleKind::Kidnapper, "kidnapper", "Похититель", Team::Mafia,
             "Мафиози. Ночью может назвать игрока и его роль: угадал — тот погибает (не лечится), "
             "ошибся — гибнет сам. Без Босса и других мафиози сам становится Боссом."},
    RoleInfo{RoleKind::Foundling, "foundling", "Подкидыш", Team::Civilians,
             "Мирный, о себе не знает. Мафия не может его убить. После смерти Босса становится мафиози."},
    RoleInfo{RoleKind::Bull, "bull", "Бык", Team::Mafia,
             "Мафиози, неуязвимый для маньяка."},
    RoleInfo{RoleKind::Paranoid, "paranoid", "Параноик", Team::Mafia,
             "Мафиози. Узнаёт, что его проверял комиссар (но не кто)."},

    RoleInfo{RoleKind::Poisoner, "poisoner", "Отравитель", Team::Loner,
             "Одиночка. Ночью травит (жертва умирает следующей ночью) или даёт противоядие отравленному. "
             "Доктор спасает, только если лечил в ночь отравления. Отравив двух мирных, умирает сам."},
    RoleInfo{RoleKind::Hacker, "hacker", "Хакер", Team::Neutral,
             "Ночью взламывает игрока: его роль ведущий утром объявляет всем."},
    RoleInfo{RoleKind::Swindler, "swindler", "Жулик", Team::Neutral,
             "Ночью может украсть ход ролевого игрока: тот ход не делает, а действие совершается от имени жулика."},
    RoleInfo{RoleKind::Werewolf, "werewolf", "Оборотень", Team::Mafia,
             "Мафия его знает, для проверки он мирный. Пока жива другая мафия — без способностей; "
             "после смерти последнего мафиози становится полноправным мафиози."},
};

inline constexpr std::size_t kRoleCount = kRoleTable.size();

// Таблица полна и упорядочена так же, как перечисление: строка i — роль i.
consteval bool role_table_ordered() {
    for (std::size_t i = 0; i < kRoleTable.size(); ++i) {
        if (static_cast<std::size_t>(kRoleTable[i].kind) != i) {
            return false;
        }
    }
    return static_cast<std::size_t>(RoleKind::Werewolf) + 1 == kRoleTable.size();
}
static_assert(role_table_ordered(), "kRoleTable должна перечислять роли в порядке RoleKind, без пропусков");

constexpr const RoleInfo& role_info(RoleKind role) noexcept {
    return kRoleTable[static_cast<std::size_t>(role)];
}

// Все роли — для перебора (каталог, фабрика, тесты).
inline constexpr std::array<RoleKind, kRoleCount> kAllRoles = [] {
    std::array<RoleKind, kRoleCount> all{};
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        all[i] = kRoleTable[i].kind;
    }
    return all;
}();

// Обязательные роли — те, что есть в любой партии.
inline constexpr std::array<RoleKind, 5> kBaseRoles = {
    RoleKind::Civilian, RoleKind::Mafioso, RoleKind::Commissar, RoleKind::Doctor, RoleKind::Maniac,
};

constexpr Team team_of(RoleKind role) noexcept {
    return role_info(role).team;
}

constexpr std::string_view role_name(RoleKind role) noexcept {
    return role_info(role).name;
}

constexpr std::string_view role_id(RoleKind role) noexcept {
    return role_info(role).id;
}

constexpr std::string_view role_description(RoleKind role) noexcept {
    return role_info(role).description;
}

// Роль по имени из конфига — латинскому id или русскому названию.
constexpr std::optional<RoleKind> role_by_name(std::string_view name) noexcept {
    for (const RoleInfo& r : kRoleTable) {
        if (r.id == name || r.name == name) {
            return r.kind;
        }
    }
    return std::nullopt;
}

// Ночные действия. Нужны и в протоколе (ответ игрока), и в тайном журнале
// ночей (GameState::night_log), поэтому лежат здесь, а не в protocol.hpp.
enum class NightActionKind {
    None,      // хода нет (воздержался, если это разрешено)
    Kill,      // мафия (голос за общую жертву), киллер, маньяк, блейд
    Heal,      // доктор
    Check,     // проверка: комиссар, каттани, реанимирующий комиссар, тюремщик, священник,
               // донор, шпион, адвокат (смысл зависит от роли проверяющего)
    Shoot,     // выстрел: комиссар, каттани, адвокат (последний мафиози)
    Guard,     // охрана: телохранитель, самурай
    Visit,     // пьяница идёт в гости
    Block,     // вор блокирует способность
    Compare,   // журналист: target и second
    Listen,    // ушастик
    Watch,     // свидетель
    Revive,    // реаниматор
    Prank,     // громила
    Freeze,    // босс «замораживает»
    Kidnap,    // похититель: target и guess
    Slander,   // баламут
    Seek,      // камикадзе ищет комиссара
    Admire,    // поклонница проверяет, комиссар ли
    Poison,    // отравитель травит
    Antidote,  // отравитель даёт противоядие
    Hack,      // хакер
    Steal,     // жулик крадёт ход
};

struct NightAction {
    NightActionKind kind = NightActionKind::None;
    std::optional<PlayerId> target;
    std::optional<PlayerId> second;  // Compare: второй сравниваемый
    std::optional<RoleKind> guess;   // Kidnap: названная роль
};

constexpr std::string_view action_name(NightActionKind kind) noexcept {
    constexpr std::array<std::string_view, 22> names = {
        "нет хода", "убийство", "лечение",   "проверка",  "выстрел",     "охрана",
        "визит",    "блокировка", "сравнение", "подслушивание", "слежка", "воскрешение",
        "шутка",    "заморозка", "похищение", "оговор",    "поиск комиссара", "поклонница",
        "отравление", "противоядие", "взлом", "кража хода",
    };
    return names[static_cast<std::size_t>(kind)];
}

// Фазы партии. День N сменяется ночью N, затем наступает день N+1.
enum class Phase {
    Setup,       // раздача ролей, знакомство
    Discussion,  // дневное обсуждение
    Voting,      // дневное голосование (включая переголосования)
    Night,       // ночные ходы
    Morning,     // утро: объявление ночных смертей, последние слова
    Finished,    // игра окончена
};

// В switch нет ветки default: если добавить значение в enum и забыть его здесь,
// -Wswitch (входит в -Wall) укажет на это место.

constexpr std::string_view team_name(Team team) noexcept {
    switch (team) {
        case Team::Civilians:
            return "Мирные";
        case Team::Mafia:
            return "Мафия";
        case Team::Loner:
            return "Одиночка";
        case Team::Neutral:
            return "Нейтральные";
    }
    return "?";
}

constexpr std::string_view phase_name(Phase phase) noexcept {
    switch (phase) {
        case Phase::Setup:
            return "Раздача ролей";
        case Phase::Discussion:
            return "Дневное обсуждение";
        case Phase::Voting:
            return "Дневное голосование";
        case Phase::Night:
            return "Ночь";
        case Phase::Morning:
            return "Утро";
        case Phase::Finished:
            return "Игра окончена";
    }
    return "?";
}

}  // namespace mafia
