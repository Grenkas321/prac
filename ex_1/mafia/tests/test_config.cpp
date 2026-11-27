// Тесты своего разбора YAML и конфига партии: поддерживаемое подмножество,
// понятные ошибки с номером строки, неизвестные ключи и роли.
#include "cli/config.hpp"
#include "cli/yaml.hpp"
#include "test_framework.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace mafia;

namespace {

// Текст ошибки разбора YAML (пусто — ошибки не было).
std::string yaml_error(std::string_view text) {
    try {
        parse_yaml(text);
    } catch (const YamlError& e) {
        return e.what();
    }
    return {};
}

std::string config_error(std::string_view text) {
    try {
        parse_config(text);
    } catch (const ConfigError& e) {
        return e.what();
    }
    return {};
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

void yaml_subset() {
    const auto entries = parse_yaml(
        "# комментарий\n"
        "---\n"
        "k: 4   # делитель\n"
        "\n"
        "name: \"Мафия: финал\"\n"
        "quote: 'it''s'\n"
        "flow: [a, \"b c\", d]\n"
        "empty_flow: []\n"
        "block:\n"
        "  - x   # первый\n"
        "  - 'y z'\n"
        "tag: a#b\n"
        "crlf: yes\r\n");
    CHECK(entries.size() == 8);
    CHECK(entries[0].key == "k" && std::get<YamlScalar>(entries[0].value) == "4" && entries[0].line == 3);
    CHECK(std::get<YamlScalar>(entries[1].value) == "Мафия: финал");  // двоеточие в кавычках — не ключ
    CHECK(std::get<YamlScalar>(entries[2].value) == "it's");
    CHECK(std::get<YamlList>(entries[3].value) == YamlList({"a", "b c", "d"}));
    CHECK(std::get<YamlList>(entries[4].value).empty());
    CHECK(std::get<YamlList>(entries[5].value) == YamlList({"x", "y z"}));
    CHECK(std::get<YamlScalar>(entries[6].value) == "a#b");  // # без пробела — часть значения
    CHECK(std::get<YamlScalar>(entries[7].value) == "yes");  // \r из Windows отброшен
}

void yaml_errors_have_positions() {
    CHECK(has(yaml_error("k: 3\n\tx: 1\n"), "строка 2, столбец 1") && has(yaml_error("\tk: 3"), "табуляция"));
    CHECK(has(yaml_error("k: 3\nk: 4\n"), "повторяется"));
    CHECK(has(yaml_error("roles:\n  inner: 1\n"), "вложенные словари"));
    CHECK(has(yaml_error("- a\n"), "с отступом"));
    CHECK(has(yaml_error("k 3\n"), "двоеточие"));
    CHECK(has(yaml_error("k:3\n"), "нужен пробел"));
    CHECK(has(yaml_error("x: \"open\n"), "не закрыта кавычка"));
    CHECK(has(yaml_error("x: [a, b\n"), "«,» или «]»"));
    CHECK(has(yaml_error("x: &anchor 1\n"), "не поддерживается"));
    CHECK(has(yaml_error("x: |\n  text\n"), "не поддерживается"));
    CHECK(has(yaml_error("x: 1 2 # ok\nnext: \"a\" b\n"), "строка 2"));
}

void config_reading() {
    const GameConfig c = parse_config(
        "k: 4\n"
        "extra_roles:\n"
        "  - bodyguard\n"
        "  - Пьяница\n"  // русское название тоже годится
        "  - killer\n");
    CHECK(c.mafia_divisor == 4);
    CHECK(c.extra_roles == std::vector<RoleKind>({RoleKind::Bodyguard, RoleKind::Drunk, RoleKind::Killer}));
    CHECK(parse_config("extra_roles: [ninja, hacker]\n").extra_roles.size() == 2);
    CHECK(!parse_config("# пусто\n").mafia_divisor);

    // Все 35 доп. ролей из задания узнаются по латинскому имени.
    std::string all = "extra_roles:\n";
    int extra = 0;
    for (const RoleInfo& r : kRoleTable) {
        if (std::ranges::find(kBaseRoles, r.kind) == kBaseRoles.end()) {
            all += "  - " + std::string(r.id) + "\n";
            ++extra;
        }
    }
    CHECK(extra == 35);
    CHECK(parse_config(all).extra_roles.size() == 35);
}

void config_errors() {
    CHECK(has(config_error("players: 10\n"), "неизвестный ключ «players»"));
    CHECK(has(config_error("k: 2\n"), "не меньше 3"));
    CHECK(has(config_error("k: three\n"), "целым числом"));
    CHECK(has(config_error("k: [3]\n"), "не списком"));
    CHECK(has(config_error("extra_roles: [dragon]\n"), "неизвестная роль «dragon»"));
    CHECK(has(config_error("extra_roles: [doctor]\n"), "обязательная роль"));
    CHECK(has(config_error("extra_roles: [spy, spy]\n"), "дважды"));
    CHECK(has(config_error("extra_roles: spy\n"), "это список"));
    CHECK(has(config_error("k: 3\n\nextra_roles:\n  - spy\n  - ???\n"), "строка 3"));  // строка ключа
    CHECK(has(config_error("k: 3\nk: 4\n"), "строка 2"));                                // синтаксис — тоже со строкой
}

void config_from_file() {
    const auto file = std::filesystem::temp_directory_path() / "mafia_test_config.yaml";
    {
        std::ofstream out(file);
        out << "k: 5\nextra_roles: [samurai]\n";
    }
    const GameConfig c = load_config(file);
    CHECK(c.mafia_divisor == 5 && c.extra_roles == std::vector<RoleKind>({RoleKind::Samurai}));
    std::filesystem::remove(file);
    bool missing = false;
    try {
        load_config(file);
    } catch (const ConfigError&) {
        missing = true;
    }
    CHECK(missing);
}

int main() {
    std::cout << "Конфиг и YAML\n";
    test::run("поддерживаемое подмножество YAML", &yaml_subset);
    test::run("синтаксические ошибки — со строкой и столбцом", &yaml_errors_have_positions);
    test::run("чтение конфига: k и дополнительные роли", &config_reading);
    test::run("ошибки конфига", &config_errors);
    test::run("конфиг из файла", &config_from_file);
    return test::finish();
}
