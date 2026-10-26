#include "st/lexer.h"

#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <system_error>

namespace simpleplc::st {

std::string toUpper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

namespace {

bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentChar(char c) { return isLetter(c) || isDigit(c) || c == '_'; }

int digitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

/// Length of the UTF-8 sequence at s[i] (1-4), or 0 when it is not valid UTF-8 (overlong forms,
/// surrogates and values above U+10FFFF included). `cp` gets the code point.
size_t utf8Length(std::string_view s, size_t i, uint32_t* cp = nullptr) {
    auto byte = [&](size_t k) { return i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0u; };
    auto cont = [&](size_t k) { return (byte(k) & 0xC0) == 0x80; };
    unsigned b0 = byte(0);
    size_t n = 0;
    uint32_t v = 0;
    if (b0 < 0x80) {
        n = 1;
        v = b0;
    } else if (b0 >= 0xC2 && b0 <= 0xDF && cont(1)) {
        n = 2;
        v = b0 & 0x1F;
    } else if (b0 >= 0xE0 && b0 <= 0xEF && cont(1) && cont(2) && !(b0 == 0xE0 && byte(1) < 0xA0) && !(b0 == 0xED && byte(1) > 0x9F)) {
        n = 3;
        v = b0 & 0x0F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4 && cont(1) && cont(2) && cont(3) && !(b0 == 0xF0 && byte(1) < 0x90) &&
               !(b0 == 0xF4 && byte(1) > 0x8F)) {
        n = 4;
        v = b0 & 0x07;
    }
    for (size_t k = 1; k < n; k++) v = (v << 6) | (byte(k) & 0x3F);
    if (cp) *cp = v;
    return n;
}

/// "U+00A0 (no-break space)" and what to write instead, for characters that look like ASCII.
std::pair<std::string, std::string> describeCharacter(uint32_t cp) {
    std::string code = std::format("U+{:04X}", cp);
    switch (cp) {
        case 0x00A0: return {code + " (no-break space)", "use a normal space"};
        case 0xFEFF: return {code + " (byte-order mark)", "remove it; only the start of a file may have one"};
        case 0x2018: return {code + " (left single quotation mark)", "strings are written with the ASCII quote '"};
        case 0x2019: return {code + " (right single quotation mark)", "strings are written with the ASCII quote '"};
        case 0x201C: return {code + " (left double quotation mark)", "strings are written with the ASCII quote '"};
        case 0x201D: return {code + " (right double quotation mark)", "strings are written with the ASCII quote '"};
        case 0x2013: return {code + " (en dash)", "use the ASCII minus -"};
        case 0x2014: return {code + " (em dash)", "use the ASCII minus -"};
        case 0x2212: return {code + " (minus sign)", "use the ASCII minus -"};
        case 0x00D7: return {code + " (multiplication sign)", "use *"};
        case 0x200B: return {code + " (zero-width space)", "remove it"};
        default: return {code, "names may use only the letters A-Z, digits and '_'"};
    }
}

bool isTimePrefix(std::string_view p) { return p == "T" || p == "TIME" || p == "LT" || p == "LTIME"; }

bool isDatePrefix(std::string_view p) {
    return p == "D" || p == "DATE" || p == "LD" || p == "LDATE" || p == "TOD" || p == "TIME_OF_DAY" ||
           p == "LTOD" || p == "LTIME_OF_DAY" || p == "DT" || p == "DATE_AND_TIME" || p == "LDT" ||
           p == "LDATE_AND_TIME";
}

class Lexer {
public:
    Lexer(std::string_view source, uint16_t file, Diagnostics& diags, bool internal)
        : src_(source), file_(file), diags_(diags), internal_(internal) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        // A UTF-8 byte-order mark (Windows editors add one) says what the text is anyway
        if (src_.starts_with("\xEF\xBB\xBF")) pos_ = lineStart_ = 3;
        // Source text is UTF-8: other bytes are refused rather than guessed (Latin-1 text would change
        // its string literals on the way through the API and the editor)
        for (size_t i = pos_; i < src_.size();) {
            size_t n = utf8Length(src_, i);
            if (n == 0) {
                error("E_SYNTAX", locAt(i), std::format("the file is not valid UTF-8 (byte 0x{:02X})", static_cast<unsigned char>(src_[i])),
                      "save the file as UTF-8");
                break;
            }
            i += n;
        }
        while (true) {
            bool spaced = skipTrivia();
            Token t = next();
            t.followsSpace = spaced;
            bool end = t.kind == Tok::End;
            out.push_back(std::move(t));
            if (end || diags_.full()) break;
        }
        if (out.back().kind != Tok::End) {
            Token end;
            end.loc = loc();
            out.push_back(std::move(end));
        }
        return out;
    }

private:
    char peek(size_t k = 0) const { return pos_ + k < src_.size() ? src_[pos_ + k] : '\0'; }
    bool atEnd() const { return pos_ >= src_.size(); }

    void advance() {
        if (atEnd()) return;
        // Line ends: LF, CRLF (counted at the LF) and a lone CR (old Mac files)
        if (src_[pos_] == '\n' || (src_[pos_] == '\r' && peek(1) != '\n')) {
            line_++;
            lineStart_ = pos_ + 1;
            continuations_ = 0;
        } else if ((static_cast<unsigned char>(src_[pos_]) & 0xC0) == 0x80) {
            continuations_++;  // not a character of its own: columns count characters
        }
        pos_++;
    }

    /// Columns count characters (Unicode code points), as an editor does, not bytes.
    SourceLoc loc() const {
        return SourceLoc{file_, line_, static_cast<uint32_t>(pos_ - lineStart_ - continuations_ + 1)};
    }

    /// The position of byte `at`, counted from the start (for a problem found before lexing).
    SourceLoc locAt(size_t at) const {
        uint32_t line = 1;
        size_t start = src_.starts_with("\xEF\xBB\xBF") ? 3 : 0;
        size_t column = 1;
        for (size_t i = start; i < at && i < src_.size(); i++) {
            if (src_[i] == '\n' || (src_[i] == '\r' && (i + 1 >= src_.size() || src_[i + 1] != '\n'))) {
                line++;
                column = 1;
            } else if ((static_cast<unsigned char>(src_[i]) & 0xC0) != 0x80) {
                column++;
            }
        }
        return SourceLoc{file_, line, static_cast<uint32_t>(column)};
    }

    void error(std::string_view code, SourceLoc at, std::string message, std::string hint = {}) {
        diags_.error(code, at, std::move(message), std::move(hint));
    }

    /// Skips whitespace, comments and pragmas; returns true if anything was skipped.
    bool skipTrivia() {
        bool skipped = false;
        while (!atEnd()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (!atEnd() && peek() != '\n' && peek() != '\r') advance();
            } else if (c == '(' && peek(1) == '*') {
                skipBlockComment('(', ')');
            } else if (c == '/' && peek(1) == '*') {
                skipBlockComment('/', '/');
            } else if (c == '{') {
                skipPragma();
            } else {
                break;
            }
            skipped = true;
        }
        return skipped;
    }

    /// `(* … *)` or `/* … */`. Comments do not nest: an inner opener is an error,
    /// because whether the text after it is live code depends on the reader.
    void skipBlockComment(char open, char close) {
        SourceLoc start = loc();
        advance();
        advance();
        while (!atEnd()) {
            if (peek() == '*' && peek(1) == close) {
                advance();
                advance();
                return;
            }
            if (peek() == open && peek(1) == '*') {
                error("E_COMMENT", loc(),
                      std::format("'{}*' inside a comment: comments do not nest", open),
                      std::format("end the outer comment first, or write the text without '{}*'", open));
            }
            advance();
        }
        error("E_COMMENT", start, "comment is not closed",
              open == '(' ? "close it with '*)'" : "close it with '*/'");
    }

    void skipPragma() {
        SourceLoc start = loc();
        advance();
        size_t bodyStart = pos_;
        while (!atEnd() && peek() != '}') advance();
        if (atEnd()) {
            error("E_SYNTAX", start, "pragma '{' is not closed", "close it with '}'");
            return;
        }
        std::string body = toUpper(src_.substr(bodyStart, pos_ - bodyStart));
        advance();
        size_t first = body.find_first_not_of(" \t\r\n");
        std::string word = first == std::string::npos ? "" : body.substr(first, body.find_first_of(" \t\r\n('", first) - first);
        if (word == "IF" || word == "ELSIF" || word == "ELSE" || word == "END_IF" || word == "DEFINE" || word == "UNDEFINE") {
            // Ignoring conditional compilation would compile both branches: a silent change of meaning
            error("E_PRAGMA", start, std::format("conditional-compilation pragma {{{}}} is not supported", word),
                  "remove the pragmas and keep the code that should run");
            return;
        }
        if (word == "ERROR") {
            error("E_PRAGMA", start, "{error …} pragma: the author marked this code as not to be compiled",
                  "remove the pragma once the code may run");
            return;
        }
        // Metadata for an editor or a tool: ignoring it does not change what the program does
        static constexpr std::string_view kMetadata[] = {"", "ATTRIBUTE", "REGION", "ENDREGION", "INFO", "WARNING",
                                                         "TEXT", "LIBRARY", "MESSAGEGUID"};
        bool metadata = isAttributeList(body);
        for (std::string_view m : kMetadata) metadata = metadata || word == m;
        if (!metadata) {
            // OpenPLC and MatIEC copy { … } into the generated C program: ignoring it would drop code that was meant to run
            error("E_PRAGMA", start, "unknown pragma: SimplePLC cannot tell what it would do, so it does not ignore it",
                  "OpenPLC runs the text between { } as C code; write it in ST instead (the cycle's time is TIME()). "
                  "Comments are written (* … *)");
            return;
        }
        diags_.warning("W_PRAGMA", start, "pragma ignored: SimplePLC has no implementation-specific pragmas");
    }

    /// Siemens' block attributes: name := 'value', separated by ';' ({S7_Optimized_Access := 'TRUE'}).
    static bool isAttributeList(std::string_view body) {
        size_t i = 0;
        auto blanks = [&] { while (i < body.size() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) i++; };
        bool any = false;
        while (true) {
            blanks();
            if (i == body.size()) return any;
            size_t name = i;
            while (i < body.size() && (std::isalnum(static_cast<unsigned char>(body[i])) || body[i] == '_')) i++;
            if (i == name) return false;
            blanks();
            if (body.substr(i, 2) != ":=") return false;
            i += 2;
            blanks();
            if (i == body.size() || body[i] != '\'') return false;
            size_t close = body.find('\'', i + 1);
            if (close == std::string_view::npos) return false;
            i = close + 1;
            any = true;
            blanks();
            if (i < body.size() && body[i] == ';') i++;
        }
    }

    Token make(Tok kind, size_t begin, SourceLoc at) const {
        Token t;
        t.kind = kind;
        t.loc = at;
        t.raw = src_.substr(begin, pos_ - begin);
        return t;
    }

    Token next() {
        SourceLoc at = loc();
        size_t begin = pos_;
        if (atEnd()) return make(Tok::End, begin, at);
        char c = peek();

        if (isLetter(c) || c == '_') return identifierOrTypedLiteral(begin, at);
        if (isDigit(c)) return number(begin, at);
        if (c == '\'') {
            Token t = make(Tok::StringLiteral, begin, at);
            t.text = stringBody('\'');
            t.raw = src_.substr(begin, pos_ - begin);
            return t;
        }
        if (c == '"') {
            Token t = make(Tok::WStringLiteral, begin, at);
            t.text = stringBody('"');
            t.raw = src_.substr(begin, pos_ - begin);
            return t;
        }

        auto one = [&](Tok kind) {
            advance();
            return make(kind, begin, at);
        };
        auto two = [&](Tok kind) {
            advance();
            advance();
            return make(kind, begin, at);
        };
        switch (c) {
            case '(': return one(Tok::LParen);
            case ')': return one(Tok::RParen);
            case '[': return one(Tok::LBracket);
            case ']': return one(Tok::RBracket);
            case ',': return one(Tok::Comma);
            case ';': return one(Tok::Semicolon);
            case ':': return peek(1) == '=' ? two(Tok::Assign) : one(Tok::Colon);
            case '=': return peek(1) == '>' ? two(Tok::Output) : one(Tok::Eq);
            case '.':
                if (peek(1) == '.') return two(Tok::DotDot);
                if (isDigit(peek(1)) && (begin == 0 || !isIdentChar(src_[begin - 1])) &&
                    (begin == 0 || (src_[begin - 1] != ']' && src_[begin - 1] != ')'))) {
                    error("E_LITERAL", at, "a real literal needs digits before the decimal point", "write 0.5, not .5");
                    advance();
                    std::string ignoredClean;
                    bool big = false;
                    uint64_t ignored = 0;
                    digits(10, ignoredClean, big, ignored);
                    return make(Tok::RealLiteral, begin, at);
                }
                return one(Tok::Dot);
            case '+': return one(Tok::Plus);
            case '-': return one(Tok::Minus);
            case '*': return peek(1) == '*' ? two(Tok::Power) : one(Tok::Star);
            case '/': return one(Tok::Slash);
            case '<':
                if (peek(1) == '=') return two(Tok::Le);
                if (peek(1) == '>') return two(Tok::Ne);
                return one(Tok::Lt);
            case '>': return peek(1) == '=' ? two(Tok::Ge) : one(Tok::Gt);
            case '&': return one(Tok::Ampersand);
            case '^': return one(Tok::Caret);
            case '@': return one(Tok::At);
            case '#': return one(Tok::Hash);
            case '%': return one(Tok::Percent);
            case '!':
                if (peek(1) == '=') {
                    error("E_SYNTAX", at, "'!=' is not Structured Text", "write '<>' for not equal");
                    return two(Tok::Ne);
                }
                break;
            default: break;
        }
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80) {
            uint32_t cp = 0;
            auto [what, hint] = utf8Length(src_, pos_, &cp) ? describeCharacter(cp) : std::pair<std::string, std::string>{
                std::format("byte 0x{:02X}", u), "save the file as UTF-8"};
            error("E_SYNTAX", at, std::format("character {} outside a string or comment: only ASCII is allowed there", what), hint);
            advance();
            while (!atEnd() && (static_cast<unsigned char>(peek()) & 0xC0) == 0x80) advance();
        } else {
            // A control character (NUL …) is named, not printed: it would cut or garble the message
            error("E_SYNTAX", at, u < 0x20 || u == 0x7F ? std::format("unexpected control character U+{:04X}", u)
                                                        : std::format("unexpected character '{}'", c));
            advance();
        }
        skipTrivia();
        return next();
    }

    Token identifierOrTypedLiteral(size_t begin, SourceLoc at) {
        while (isIdentChar(peek())) advance();
        std::string_view word = src_.substr(begin, pos_ - begin);
        std::string upper = toUpper(word);

        if (peek() == '#') {
            advance();
            return typedLiteral(begin, at, std::move(upper));
        }

        checkIdentifier(word, at);
        Tok kind = keywordKind(upper);
        Token t = make(kind, begin, at);
        t.text = std::move(upper);
        return t;
    }

    void checkIdentifier(std::string_view word, SourceLoc at) {
        if (internal_) return;
        if (word.find("__") != std::string_view::npos) {
            error("E_SYNTAX", at, std::format("'{}': two underscores in a row are not allowed in a name", word),
                  "IEC 61131-3 allows single underscores between letters and digits only");
        } else if (word.back() == '_') {
            error("E_SYNTAX", at, std::format("'{}': a name must not end with '_'", word));
        } else if (word.size() > 64) {
            error("E_LIMIT", at, std::format("name is {} characters long; the limit is 64", word.size()));
        }
    }

    Token typedLiteral(size_t begin, SourceLoc at, std::string prefix) {
        Token t;
        t.kind = Tok::TypedLiteral;
        t.loc = at;
        t.prefix = std::move(prefix);
        if (peek() == '\'' || peek() == '"') {
            t.quoted = true;
            t.text = stringBody(peek());
            t.raw = src_.substr(begin, pos_ - begin);
            return t;
        }
        size_t bodyStart = pos_;
        if (peek() == '-' || peek() == '+') advance();
        bool date = isDatePrefix(t.prefix);
        bool time = isTimePrefix(t.prefix);
        bool sawHash = false;
        while (!atEnd()) {
            char c = peek();
            if (isIdentChar(c)) {
                advance();
            } else if (c == '.' && isDigit(peek(1))) {
                advance();
            } else if (date && (c == '-' || c == ':') && isDigit(peek(1))) {
                advance();
            } else if (!time && !date && c == '#' && !sawHash) {
                // INT#16#FF: a based number inside a typed literal
                std::string_view sofar = src_.substr(bodyStart, pos_ - bodyStart);
                bool digitsOnly = !sofar.empty();
                for (char d : sofar) digitsOnly = digitsOnly && isDigit(d);
                if (!digitsOnly) break;
                sawHash = true;
                advance();
            } else if (!time && !date && (c == 'E' || c == 'e')) {
                advance();
            } else if (!time && !date && (c == '+' || c == '-') &&
                       (src_[pos_ - 1] == 'E' || src_[pos_ - 1] == 'e') && isDigit(peek(1))) {
                advance();  // exponent sign: REAL#1.5E-3
            } else {
                break;
            }
        }
        if (time && peek() == '-' && isDigit(peek(1))) {
            // T#1h-5m: a sign is allowed only right after the prefix
            error("E_LITERAL", at, "a '-' in a duration is allowed only right after T#", "write T#-1h5m for a negative duration");
            while (!atEnd() && (isIdentChar(peek()) || peek() == '-' || peek() == '.')) advance();
        }
        t.text = std::string(src_.substr(bodyStart, pos_ - bodyStart));
        t.raw = src_.substr(begin, pos_ - begin);
        if (t.text.empty() || t.text == "-" || t.text == "+") {
            error("E_LITERAL", at, std::format("'{}#' must be followed by a value", t.prefix));
        }
        return t;
    }

    /// Scans digits of `base` with single underscores between digits; returns false on a misplaced '_'.
    bool digits(int base, std::string& clean, bool& tooBig, uint64_t& value) {
        bool ok = true;
        bool any = false;
        while (!atEnd()) {
            char c = peek();
            if (c == '_') {
                if (!any || digitValue(peek(1)) >= base) ok = false;
                advance();
                continue;
            }
            int d = digitValue(c);
            if (d >= base) break;
            any = true;
            clean.push_back(c);
            uint64_t b = static_cast<uint64_t>(base);
            if (value > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(d)) / b) tooBig = true;
            value = value * b + static_cast<uint64_t>(d);
            advance();
        }
        return ok && any;
    }

    Token number(size_t begin, SourceLoc at) {
        std::string clean;
        bool tooBig = false;
        uint64_t value = 0;
        bool ok = digits(10, clean, tooBig, value);

        if (peek() == '#') {
            // Based integer: 2#1010, 8#17, 16#FF
            advance();
            uint64_t base = value;
            if (base != 2 && base != 8 && base != 16) {
                error("E_LITERAL", at, std::format("base {} is not allowed", clean), "use 2#, 8# or 16#");
                base = 16;
            }
            std::string bclean;
            uint64_t bvalue = 0;
            bool btooBig = false;
            bool bok = digits(static_cast<int>(base), bclean, btooBig, bvalue);
            if (isIdentChar(peek())) {
                error("E_LITERAL", loc(), std::format("'{}' is not a digit in base {}", peek(), base));
                while (isIdentChar(peek())) advance();
                bok = true;
            } else if (!bok) {
                error("E_LITERAL", at, "based literal needs digits, with '_' only between digits");
            }
            Token t = make(Tok::IntLiteral, begin, at);
            if (btooBig) error("E_LITERAL_RANGE", at, "integer literal is larger than 64 bits");
            t.intValue = bvalue;
            return t;
        }

        bool real = false;
        if (peek() == '.' && !isDigit(peek(1)) && peek(1) != '.' && !isIdentChar(peek(1))) {
            error("E_LITERAL", at, "a real literal needs digits after the decimal point",
                  std::format("write {}.0", clean));
            advance();
            Token t = make(Tok::RealLiteral, begin, at);
            t.realValue = static_cast<double>(value);
            return t;
        }
        if (peek() == '.' && isDigit(peek(1))) {
            real = true;
            clean.push_back('.');
            advance();
            uint64_t ignored = 0;
            bool fracBig = false;
            if (!digits(10, clean, fracBig, ignored)) ok = false;
        }
        bool exponent = (peek() == 'E' || peek() == 'e') &&
                        (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))));
        if (exponent) {
            if (!real) {
                error("E_LITERAL", at, "a real literal needs a decimal point",
                      std::format("write {}.0E…", clean));
                real = true;
            }
            clean.push_back('E');
            advance();
            if (peek() == '+' || peek() == '-') {
                clean.push_back(peek());
                advance();
            }
            uint64_t ignored = 0;
            bool expBig = false;
            if (!digits(10, clean, expBig, ignored)) ok = false;
        }
        if (!ok) error("E_LITERAL", at, "'_' is allowed only between digits");
        if (isIdentChar(peek())) {
            error("E_SYNTAX", loc(), "a number must be followed by a space or an operator");
            while (isIdentChar(peek())) advance();
        }

        if (real) {
            Token t = make(Tok::RealLiteral, begin, at);
            double d = 0;
            auto [ptr, ec] = std::from_chars(clean.data(), clean.data() + clean.size(), d);
            if (ec == std::errc::result_out_of_range || ptr != clean.data() + clean.size()) {
                error("E_LITERAL_RANGE", at, "real literal is outside the range of LREAL");
            }
            t.realValue = d;
            return t;
        }
        Token t = make(Tok::IntLiteral, begin, at);
        if (tooBig) error("E_LITERAL_RANGE", at, "integer literal is larger than 64 bits");
        t.intValue = value;
        return t;
    }

    /// A '…' or "…" literal starting at the quote; returns the decoded bytes.
    std::string stringBody(char quote) {
        SourceLoc start = loc();
        advance();
        std::string out;
        while (true) {
            if (atEnd() || peek() == '\n' || peek() == '\r') {
                error("E_LITERAL", start, "string literal is not closed on this line",
                      "close it with the same quote; write $N for a line break");
                return out;
            }
            char c = peek();
            if (c == quote) {
                advance();
                return out;
            }
            if (c == '$') {
                SourceLoc at = loc();
                advance();
                char e = peek();
                char upper = (e >= 'a' && e <= 'z') ? static_cast<char>(e - 'a' + 'A') : e;
                switch (upper) {
                    case '$': out.push_back('$'); advance(); break;
                    case 'L': case 'N': out.push_back('\n'); advance(); break;
                    case 'P': out.push_back('\f'); advance(); break;
                    case 'R': out.push_back('\r'); advance(); break;
                    case 'T': out.push_back('\t'); advance(); break;
                    case '\'':
                    case '"':
                        if (e != quote) {
                            error("E_LITERAL", at, std::format("'${}' is not an escape in this kind of string", e),
                                  std::format("write the character {} directly", e));
                        }
                        out.push_back(e);
                        advance();
                        break;
                    default:
                        if (digitValue(e) < 16 && digitValue(peek(1)) < 16) {
                            out.push_back(static_cast<char>(digitValue(e) * 16 + digitValue(peek(1))));
                            advance();
                            advance();
                        } else {
                            error("E_LITERAL", at, "unknown escape after '$'",
                                  "use $$, $', $L, $N, $P, $R, $T or $ with two hex digits");
                        }
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
                error("E_LITERAL", loc(), "control character inside a string literal", "write $T for a tab");
            }
            out.push_back(c);
            advance();
        }
    }

    std::string_view src_;
    uint16_t file_;
    Diagnostics& diags_;
    bool internal_;
    size_t pos_ = 0;
    uint32_t line_ = 1;
    size_t lineStart_ = 0;
    size_t continuations_ = 0;  // UTF-8 continuation bytes between lineStart_ and pos_
};

}  // namespace

std::vector<Token> lex(std::string_view source, uint16_t fileIndex, Diagnostics& diags, bool internal) {
    return Lexer(source, fileIndex, diags, internal).run();
}

}  // namespace simpleplc::st
