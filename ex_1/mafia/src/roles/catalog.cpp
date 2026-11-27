// Контракт ролей: что каждая роль обязана уметь и чего уметь НЕ должна.
// Файл входит в сборку программы, поэтому нарушение контракта ломает
// `make build`, а не всплывает посреди партии.
//
// Отрицательные проверки так же важны, как положительные: роли наследуются
// (Doctor : Civilian), и способность, случайно добавленная в базу,
// «расползлась» бы по наследникам — например, мирный житель начал бы ходить ночью.
#include "roles/catalog.hpp"

namespace mafia {

// Мирный житель: ночью спит, тайн не получает.
static_assert(RoleClass<Civilian>);
static_assert(!NightActor<Civilian>);
static_assert(!SecretReceiver<Civilian>);
static_assert(!MafiaMember<Civilian> && !Loner<Civilian>);

// Комиссар: проверяет или стреляет; результат проверки ему доставляется.
static_assert(Inspector<Commissar>);
static_assert(Killer<Commissar>);
static_assert(!Protector<Commissar>);
static_assert(!Commissar::kMaySkipNight);

// Доктор: только защищает.
static_assert(Protector<Doctor>);
static_assert(!Killer<Doctor> && !Inspector<Doctor>);
static_assert(!SecretReceiver<Doctor>);
static_assert(!Doctor::kMaySkipNight);

// Мафиози: член мафии, убивает, знает союзников.
static_assert(MafiaMember<Mafioso>);
static_assert(Killer<Mafioso>);
static_assert(SecretReceiver<Mafioso>);
static_assert(!Inspector<Mafioso> && !Protector<Mafioso>);

// Маньяк: одиночка, убивает, ничего тайного не узнаёт.
static_assert(Loner<Maniac>);
static_assert(Killer<Maniac>);
static_assert(!SecretReceiver<Maniac> && !MafiaMember<Maniac>);

// База ролью не является: у неё нет собственного вида.
static_assert(!RoleClass<Player>);

// Каталог полон и согласован с перечислением RoleKind.
static_assert(catalog_complete());

// Таблица для ведущего совпадает с концептами.
static_assert(!role_capabilities(RoleKind::Civilian).night_actor);
static_assert(role_capabilities(RoleKind::Commissar).inspector);
static_assert(role_capabilities(RoleKind::Doctor).protector);
static_assert(role_capabilities(RoleKind::Mafioso).receives_secrets);
static_assert(role_capabilities(RoleKind::Maniac).killer);

}  // namespace mafia
