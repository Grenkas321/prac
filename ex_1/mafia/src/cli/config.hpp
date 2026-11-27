// Конфиг партии (YAML, свой разбор — cli/yaml.hpp).
//
//   # делитель числа мафии
//   k: 3
//   # дополнительные роли: латинские имена или русские названия
//   extra_roles:
//     - bodyguard
//     - Пьяница
//     - killer
//
// Неизвестный ключ, неизвестная или повторная роль, обязательная роль в списке
// дополнительных, k < 3 — ошибка ConfigError с номером строки.
#pragma once

#include "game/types.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace mafia {

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct GameConfig {
    std::optional<int> mafia_divisor;  // k; пусто — не задан в конфиге
    std::vector<RoleKind> extra_roles;
};

GameConfig parse_config(std::string_view text);
GameConfig load_config(const std::filesystem::path& file);

}  // namespace mafia
