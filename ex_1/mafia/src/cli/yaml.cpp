#include "cli/yaml.hpp"

#include <set>

namespace mafia {

YamlError::YamlError(std::size_t line, std::size_t column, const std::string& message)
    : std::runtime_error("строка " + std::to_string(line) + ", столбец " + std::to_string(column) + ": " + message),
      line_(line),
      column_(column) {}

namespace {

bool is_key_char(char c, bool first) {
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    return first ? alpha : alpha || (c >= '0' && c <= '9') || c == '-';
}

// Разбор одной строки: позиция pos двигается по тексту строки; номер строки —
// для сообщений. Столбцы в сообщениях — с 1, в байтах.
class LineParser {
public:
    LineParser(std::string_view text, std::size_t line) : text_(text), line_(line) {}

    [[noreturn]] void fail(const std::string& message) const { throw YamlError(line_, pos_ + 1, message); }

    void skip_spaces() {
        while (pos_ < text_.size() && text_[pos_] == ' ') {
            ++pos_;
        }
    }
    bool at_end_or_comment() {
        skip_spaces();
        return pos_ >= text_.size() || text_[pos_] == '#';
    }
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    std::size_t pos() const { return pos_; }
    void advance(std::size_t n = 1) { pos_ += n; }

    // Скаляр: в кавычках или «простой» до конца строки / комментария / разделителя.
    std::string scalar(std::string_view stops) {
        skip_spaces();
        const char c = peek();
        if (c == '"' || c == '\'') {
            return quoted(c);
        }
        if (c == '&' || c == '*' || c == '!' || c == '|' || c == '>' || c == '{') {
            fail(std::string("конструкция «") + c + "» не поддерживается этим разбором YAML");
        }
        const std::size_t start = pos_;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (stops.find(ch) != std::string_view::npos) {
                break;
            }
            if (ch == '#' && pos_ > start && text_[pos_ - 1] == ' ') {
                break;  // комментарий только после пробела: a#b — это значение
            }
            ++pos_;
        }
        std::string_view raw = text_.substr(start, pos_ - start);
        while (!raw.empty() && raw.back() == ' ') {
            raw.remove_suffix(1);
        }
        return std::string(raw);
    }

private:
    std::string quoted(char quote) {
        const std::size_t open = pos_;
        ++pos_;
        std::string out;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch == quote) {
                if (quote == '\'' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '\'') {
                    out += '\'';  // '' внутри одинарных кавычек — это одна кавычка
                    pos_ += 2;
                    continue;
                }
                ++pos_;
                return out;
            }
            if (quote == '"' && ch == '\\' && pos_ + 1 < text_.size()) {
                const char e = text_[pos_ + 1];
                if (e == '"' || e == '\\') {
                    out += e;
                } else if (e == 'n') {
                    out += '\n';
                } else if (e == 't') {
                    out += '\t';
                } else {
                    fail(std::string("неизвестная экранирующая последовательность \\") + e);
                }
                pos_ += 2;
                continue;
            }
            out += ch;
            ++pos_;
        }
        pos_ = open;
        fail("не закрыта кавычка");
    }

    std::string_view text_;
    std::size_t line_;
    std::size_t pos_ = 0;
};

}  // namespace

std::vector<YamlEntry> parse_yaml(std::string_view text) {
    std::vector<YamlEntry> entries;
    std::set<std::string> keys;
    bool block_list_open = false;  // последний ключ без значения ждёт «- элемент»

    std::size_t line_no = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);  // файлы из Windows
        }

        LineParser p(line, line_no);
        // Отступ — только пробелами.
        std::size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) {
            if (line[indent] == '\t') {
                throw YamlError(line_no, indent + 1, "табуляция в отступе запрещена в YAML — используйте пробелы");
            }
            ++indent;
        }
        p.advance(indent);
        if (p.at_end_or_comment()) {
            continue;  // пустая строка или комментарий
        }
        if (indent == 0 && line.substr(0, 3) == "---" && line.find_first_not_of('-') == std::string_view::npos) {
            continue;  // начало документа
        }

        // Элемент списка блоком: «  - значение».
        if (p.peek() == '-' && (p.pos() + 1 == line.size() || line[p.pos() + 1] == ' ')) {
            if (indent == 0) {
                p.fail("элемент списка «-» должен быть с отступом под своим ключом");
            }
            if (!block_list_open) {
                p.fail("элемент списка без ключа — перед ним должна быть строка «ключ:»");
            }
            p.advance();
            std::string item = p.scalar("");
            if (item.empty()) {
                p.fail("пустой элемент списка");
            }
            if (!p.at_end_or_comment()) {
                p.fail("лишний текст после элемента списка");
            }
            auto& list = std::get<YamlList>(entries.back().value);
            list.push_back(std::move(item));
            continue;
        }

        if (indent != 0) {
            p.fail("вложенные словари не поддерживаются: ключ должен начинаться с первого столбца");
        }

        // «ключ: ...»
        const std::size_t key_start = p.pos();
        if (!is_key_char(p.peek(), true)) {
            p.fail("ожидалось имя ключа (латинские буквы, цифры, _ и -)");
        }
        while (is_key_char(p.peek(), false)) {
            p.advance();
        }
        std::string key(line.substr(key_start, p.pos() - key_start));
        if (p.peek() != ':') {
            p.fail("после ключа «" + key + "» ожидалось двоеточие");
        }
        p.advance();
        if (p.peek() != '\0' && p.peek() != ' ') {
            p.fail("после «" + key + ":» нужен пробел");
        }
        if (!keys.insert(key).second) {
            throw YamlError(line_no, key_start + 1, "ключ «" + key + "» повторяется");
        }

        YamlEntry entry;
        entry.key = key;
        entry.line = line_no;
        block_list_open = false;
        if (p.at_end_or_comment()) {
            entry.value = YamlList{};  // значение — список блоком на следующих строках (или пусто)
            block_list_open = true;
        } else if (p.peek() == '[') {
            p.advance();
            YamlList list;
            p.skip_spaces();
            if (p.peek() == ']') {
                p.advance();
            } else {
                for (;;) {
                    std::string item = p.scalar(",]");
                    if (item.empty()) {
                        p.fail("пустой элемент списка");
                    }
                    list.push_back(std::move(item));
                    p.skip_spaces();
                    if (p.peek() == ',') {
                        p.advance();
                        continue;
                    }
                    if (p.peek() == ']') {
                        p.advance();
                        break;
                    }
                    p.fail("в списке ожидалась «,» или «]»");
                }
            }
            if (!p.at_end_or_comment()) {
                p.fail("лишний текст после списка");
            }
            entry.value = std::move(list);
        } else {
            entry.value = p.scalar("");
            if (!p.at_end_or_comment()) {
                p.fail("лишний текст после значения");
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

}  // namespace mafia
