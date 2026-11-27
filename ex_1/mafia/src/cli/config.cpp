#include "cli/config.hpp"

#include "cli/yaml.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace mafia {

namespace {

std::string at_line(std::size_t line) {
    return "конфиг, строка " + std::to_string(line) + ": ";
}

}  // namespace

GameConfig parse_config(std::string_view text) {
    std::vector<YamlEntry> entries;
    try {
        entries = parse_yaml(text);
    } catch (const YamlError& e) {
        throw ConfigError(std::string("конфиг: ") + e.what());
    }

    GameConfig config;
    for (const YamlEntry& e : entries) {
        if (e.key == "k") {
            const auto* text_value = std::get_if<YamlScalar>(&e.value);
            if (!text_value) {
                throw ConfigError(at_line(e.line) + "k должен быть числом, а не списком");
            }
            int k = 0;
            const auto [end, error] = std::from_chars(text_value->data(), text_value->data() + text_value->size(), k);
            if (error != std::errc{} || end != text_value->data() + text_value->size()) {
                throw ConfigError(at_line(e.line) + "k должен быть целым числом, а не «" + *text_value + "»");
            }
            if (k < 3) {
                throw ConfigError(at_line(e.line) + "k должен быть не меньше 3, задано " + std::to_string(k));
            }
            config.mafia_divisor = k;
        } else if (e.key == "extra_roles") {
            const auto* list = std::get_if<YamlList>(&e.value);
            if (!list) {
                // Одна роль строкой — тоже понятно, что имелось в виду.
                const auto& single = std::get<YamlScalar>(e.value);
                if (single.empty()) {
                    continue;
                }
                throw ConfigError(at_line(e.line) + "extra_roles — это список: «extra_roles: [" + single + "]»");
            }
            for (const std::string& name : *list) {
                const std::optional<RoleKind> role = role_by_name(name);
                if (!role) {
                    throw ConfigError(at_line(e.line) + "неизвестная роль «" + name + "»");
                }
                if (std::ranges::find(kBaseRoles, *role) != kBaseRoles.end()) {
                    throw ConfigError(at_line(e.line) + "«" + name + "» — обязательная роль, она есть в любой партии");
                }
                if (std::ranges::find(config.extra_roles, *role) != config.extra_roles.end()) {
                    throw ConfigError(at_line(e.line) + "роль «" + name + "» указана дважды");
                }
                config.extra_roles.push_back(*role);
            }
        } else {
            throw ConfigError(at_line(e.line) + "неизвестный ключ «" + e.key + "» — допустимы k и extra_roles");
        }
    }
    return config;
}

GameConfig load_config(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        throw ConfigError("не удалось открыть конфиг " + file.string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse_config(text.str());
}

}  // namespace mafia
