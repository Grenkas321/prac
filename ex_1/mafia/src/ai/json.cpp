#include "ai/json.hpp"

#include <charconv>
#include <cstdint>

namespace mafia::json {

const Value* Value::get(std::string_view key) const {
    const Object* o = as_object();
    if (!o) {
        return nullptr;
    }
    auto it = o->find(std::string(key));
    return it == o->end() ? nullptr : &it->second;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Value document() {
        Value v = value(0);
        skip_ws();
        if (pos_ != s_.size()) {
            fail("лишние символы после значения");
        }
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw ParseError("JSON, позиция " + std::to_string(pos_) + ": " + what);
    }
    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }
    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    void expect(char c) {
        if (peek() != c) {
            fail(std::string("ожидался символ «") + c + "»");
        }
        ++pos_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) == word) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    Value value(std::size_t depth) {
        if (depth > kMaxDepth) {
            fail("слишком глубокая вложенность");
        }
        skip_ws();
        Value v;
        const char c = peek();
        if (c == '{') {
            v.data = std::make_shared<Object>(object(depth));
        } else if (c == '[') {
            v.data = std::make_shared<Array>(array(depth));
        } else if (c == '"') {
            v.data = string();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            v.data = number();
        } else if (literal("true")) {
            v.data = true;
        } else if (literal("false")) {
            v.data = false;
        } else if (literal("null")) {
            v.data = nullptr;
        } else {
            fail("ожидалось значение");
        }
        return v;
    }

    Object object(std::size_t depth) {
        expect('{');
        Object o;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return o;
        }
        for (;;) {
            skip_ws();
            std::string key = string();
            skip_ws();
            expect(':');
            o[std::move(key)] = value(depth + 1);  // повтор ключа: побеждает последний (как в большинстве разборов)
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect('}');
            return o;
        }
    }

    Array array(std::size_t depth) {
        expect('[');
        Array a;
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return a;
        }
        for (;;) {
            a.push_back(value(depth + 1));
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect(']');
            return a;
        }
    }

    double number() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        } else {
            fail("неверное число");
        }
        if (peek() == '.') {
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) {
                fail("неверное число");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            if (!(peek() >= '0' && peek() <= '9')) {
                fail("неверное число");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }
        double v = 0;
        const auto [end, err] = std::from_chars(s_.data() + start, s_.data() + pos_, v);
        if (err != std::errc{} || end != s_.data() + pos_) {
            fail("неверное число");
        }
        return v;
    }

    unsigned hex4() {
        if (pos_ + 4 > s_.size()) {
            fail("обрывается \\u");
        }
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                fail("неверная цифра в \\u");
            }
        }
        return v;
    }

    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string string() {
        expect('"');
        std::string out;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("управляющий символ внутри строки");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) {
                break;
            }
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // суррогатная пара
                        if (!(literal("\\u"))) {
                            fail("неполная суррогатная пара");
                        }
                        const std::uint32_t low = hex4();
                        if (low < 0xDC00 || low > 0xDFFF) {
                            fail("неверная суррогатная пара");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("одинокий младший суррогат");
                    }
                    utf8(out, cp);
                    break;
                }
                default:
                    fail("неизвестное экранирование");
            }
        }
        fail("не закрыта строка");
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

}  // namespace

Value parse(std::string_view text) {
    return Parser(text).document();
}

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;  // UTF-8 передаём как есть: JSON это разрешает
                }
        }
    }
    out += '"';
    return out;
}

}  // namespace mafia::json
