// Тесты концептов ролей. Главное здесь — static_assert на «сломанных» ролях:
// концепт обязан отвергнуть каждую типичную ошибку при добавлении роли.
// Методы макетов не определены: концепты проверяют объявления, тела не нужны.
#include "roles/catalog.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <array>

using namespace mafia;

namespace mock {

// Общая база макетов: конструктор, которого требует RoleClass.
struct Base : Player {
    explicit Base(PlayerSetup setup) : Player(std::move(setup), RoleKind::Civilian) {}
};

// Объявил ночные действия, но забыл act().
struct ForgotAct : Base {
    using Base::Base;
    static constexpr RoleKind kKind = RoleKind::Doctor;
    static constexpr std::array<NightActionKind, 1> kNightActions = {NightActionKind::Heal};
    static constexpr bool kMaySkipNight = false;
};

// act() есть, но возвращает не Response.
struct WrongActResult : Base {
    using Base::Base;
    static constexpr RoleKind kKind = RoleKind::Maniac;
    static constexpr std::array<NightActionKind, 1> kNightActions = {NightActionKind::Kill};
    static constexpr bool kMaySkipNight = false;
    NightAction act(const Request& request);
};

// Пустой список ночных действий.
struct EmptyNight : Base {
    using Base::Base;
    static constexpr RoleKind kKind = RoleKind::Maniac;
    static constexpr std::array<NightActionKind, 0> kNightActions = {};
    static constexpr bool kMaySkipNight = false;
    Response act(const Request& request);
};

// Забыл признак «можно ли пропустить ход».
struct NoSkipFlag : Base {
    using Base::Base;
    static constexpr RoleKind kKind = RoleKind::Maniac;
    static constexpr std::array<NightActionKind, 1> kNightActions = {NightActionKind::Kill};
    Response act(const Request& request);
};

// Проверяет, но не умеет принять результат — ведущему некуда его доставить.
struct DeafInspector : Base {
    using Base::Base;
    static constexpr RoleKind kKind = RoleKind::Commissar;
    static constexpr std::array<NightActionKind, 1> kNightActions = {NightActionKind::Check};
    static constexpr bool kMaySkipNight = false;
    Response act(const Request& request);
};

// Лечит, но не объявил правило выбора цели.
struct RulelessHealer : Base {
    using Base::Base;
    static constexpr RoleKind kKind = RoleKind::Doctor;
    static constexpr std::array<NightActionKind, 1> kNightActions = {NightActionKind::Heal};
    static constexpr bool kMaySkipNight = false;
    Response act(const Request& request);
};

// Мафиози, который не узнаёт союзников.
struct LonelyMafioso : Base {
    using Base::Base;
    static constexpr RoleKind kKind = RoleKind::Mafioso;
    static constexpr std::array<NightActionKind, 1> kNightActions = {NightActionKind::Kill};
    static constexpr bool kMaySkipNight = false;
    Response act(const Request& request);
};

// Наследник комиссара, забывший переопределить kKind: формально корректная
// роль, но каталогу он не подходит (его kKind — Commissar).
struct Sergeant : Commissar {
    using Commissar::Commissar;
};

// Вообще не роль.
struct NotARole {};

}  // namespace mock

static_assert(RoleClass<mock::ForgotAct> && !NightActor<mock::ForgotAct>);
static_assert(!NightActor<mock::WrongActResult>);
static_assert(!NightActor<mock::EmptyNight>);
static_assert(!NightActor<mock::NoSkipFlag>);
static_assert(NightActor<mock::DeafInspector> && !Inspector<mock::DeafInspector>);
static_assert(NightActor<mock::RulelessHealer> && !Protector<mock::RulelessHealer>);
static_assert(MafiaMember<mock::LonelyMafioso> && !ConsistentRole<mock::LonelyMafioso>);
static_assert(Inspector<mock::Sergeant> && mock::Sergeant::kKind != RoleKind::Doctor);
static_assert(!RoleClass<mock::NotARole>);

void capability_table() {
    // Ведущий во время игры: «кому слать ночной запрос» и «кому доставлять тайны».
    for (RoleKind kind : kAllRoles) {  // таблица строится для всех 40 ролей
        RoleCapabilities caps = role_capabilities(kind);
        CHECK(caps.kind == kind);
        CHECK(caps.night_actor == !caps.night_actions.empty());
    }
    int night_actors = 0;
    int secret_receivers = 0;
    for (RoleKind kind : kBaseRoles) {  // обязательные роли
        RoleCapabilities caps = role_capabilities(kind);
        night_actors += caps.night_actor ? 1 : 0;
        secret_receivers += caps.receives_secrets ? 1 : 0;
    }
    CHECK(night_actors == 4);      // все, кроме мирного жителя
    CHECK(secret_receivers == 2);  // комиссар (проверки) и мафиози (союзники)
    // Доп. роли: спящие до превращения, мафиозные — все знают своих.
    CHECK(role_capabilities(RoleKind::Sergeant).dormant && role_capabilities(RoleKind::Werewolf).dormant);
    CHECK(role_capabilities(RoleKind::Foundling).dormant && !role_capabilities(RoleKind::Bodyguard).dormant);
    for (RoleKind kind : kAllRoles) {
        if (team_of(kind) == Team::Mafia) {
            CHECK(role_capabilities(kind).receives_secrets);
        }
    }

    RoleCapabilities com = role_capabilities(RoleKind::Commissar);
    CHECK(com.night_actions.size() == 2);
    CHECK(std::find(com.night_actions.begin(), com.night_actions.end(), NightActionKind::Shoot) !=
          com.night_actions.end());
    CHECK(!com.may_skip_night);

    RoleCapabilities civ = role_capabilities(RoleKind::Civilian);
    CHECK(!civ.night_actor && !civ.receives_secrets && civ.night_actions.empty());
}

int main() {
    std::cout << "Концепты ролей\n";
    test::run("таблица способностей для ведущего", &capability_table);
    return test::finish();
}
