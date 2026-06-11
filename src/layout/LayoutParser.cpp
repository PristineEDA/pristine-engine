#include "pristine/layout/LayoutParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace pristine::layout {
namespace {

enum class TokenKind {
    End,
    Word,
    Number,
    String,
    Semicolon,
    LParen,
    RParen,
    Plus,
    Minus,
    Star,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    double number = 0.0;
    std::size_t line = 1;
    std::size_t column = 1;
};

std::string upper(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        result.push_back(static_cast<char>(std::toupper(ch)));
    }
    return result;
}

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    return upper(lhs) == upper(rhs);
}

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    Token next() {
        skipWhitespaceAndComments();
        if (offset_ >= text_.size()) {
            return Token{.kind = TokenKind::End, .line = line_, .column = column_};
        }

        const auto start_line = line_;
        const auto start_column = column_;
        const char ch = peek();
        switch (ch) {
            case ';':
                advance();
                return Token{.kind = TokenKind::Semicolon,
                             .text = ";",
                             .line = start_line,
                             .column = start_column};
            case '(':
                advance();
                return Token{.kind = TokenKind::LParen,
                             .text = "(",
                             .line = start_line,
                             .column = start_column};
            case ')':
                advance();
                return Token{.kind = TokenKind::RParen,
                             .text = ")",
                             .line = start_line,
                             .column = start_column};
            case '+':
                advance();
                return Token{.kind = TokenKind::Plus,
                             .text = "+",
                             .line = start_line,
                             .column = start_column};
            case '-':
                if (offset_ + 1 < text_.size() &&
                    (std::isdigit(static_cast<unsigned char>(text_[offset_ + 1])) ||
                     text_[offset_ + 1] == '.')) {
                    return readNumber();
                }
                advance();
                return Token{.kind = TokenKind::Minus,
                             .text = "-",
                             .line = start_line,
                             .column = start_column};
            case '*':
                advance();
                return Token{.kind = TokenKind::Star,
                             .text = "*",
                             .line = start_line,
                             .column = start_column};
            case '"':
                return readString();
            default:
                if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
                    return readNumber();
                }
                return readWord();
        }
    }

private:
    char peek() const { return text_[offset_]; }

    char advance() {
        const char ch = text_[offset_++];
        if (ch == '\n') {
            ++line_;
            column_ = 1;
        }
        else {
            ++column_;
        }
        return ch;
    }

    void skipWhitespaceAndComments() {
        while (offset_ < text_.size()) {
            const char ch = peek();
            if (std::isspace(static_cast<unsigned char>(ch))) {
                advance();
                continue;
            }
            if (ch == '#') {
                while (offset_ < text_.size() && peek() != '\n') {
                    advance();
                }
                continue;
            }
            if (ch == '/' && offset_ + 1 < text_.size() && text_[offset_ + 1] == '/') {
                while (offset_ < text_.size() && peek() != '\n') {
                    advance();
                }
                continue;
            }
            if (ch == '/' && offset_ + 1 < text_.size() && text_[offset_ + 1] == '*') {
                advance();
                advance();
                while (offset_ + 1 < text_.size()) {
                    if (peek() == '*' && text_[offset_ + 1] == '/') {
                        advance();
                        advance();
                        break;
                    }
                    advance();
                }
                continue;
            }
            break;
        }
    }

    Token readString() {
        const auto start_line = line_;
        const auto start_column = column_;
        advance();
        std::string value;
        while (offset_ < text_.size()) {
            const char ch = advance();
            if (ch == '"') {
                break;
            }
            if (ch == '\\' && offset_ < text_.size()) {
                value.push_back(advance());
            }
            else {
                value.push_back(ch);
            }
        }
        return Token{.kind = TokenKind::String,
                     .text = std::move(value),
                     .line = start_line,
                     .column = start_column};
    }

    Token readNumber() {
        const auto start_offset = offset_;
        const auto start_line = line_;
        const auto start_column = column_;
        if (peek() == '-') {
            advance();
        }
        while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
        if (offset_ < text_.size() && peek() == '.') {
            advance();
            while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }
        if (offset_ < text_.size() && (peek() == 'e' || peek() == 'E')) {
            advance();
            if (offset_ < text_.size() && (peek() == '+' || peek() == '-')) {
                advance();
            }
            while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }
        const auto text = std::string(text_.substr(start_offset, offset_ - start_offset));
        double value = 0.0;
        try {
            value = std::stod(text);
        }
        catch (const std::exception&) {
            value = 0.0;
        }
        return Token{.kind = TokenKind::Number,
                     .text = text,
                     .number = value,
                     .line = start_line,
                     .column = start_column};
    }

    Token readWord() {
        const auto start_line = line_;
        const auto start_column = column_;
        std::string value;
        if (peek() == '\\') {
            advance();
            while (offset_ < text_.size() && !std::isspace(static_cast<unsigned char>(peek())) &&
                   peek() != ';' && peek() != '(' && peek() != ')') {
                value.push_back(advance());
            }
            return Token{.kind = TokenKind::Word,
                         .text = std::move(value),
                         .line = start_line,
                         .column = start_column};
        }

        while (offset_ < text_.size()) {
            const char ch = peek();
            if (std::isspace(static_cast<unsigned char>(ch)) || ch == ';' || ch == '(' ||
                ch == ')' || ch == '+' || ch == '*') {
                break;
            }
            if (ch == '-' && value.empty()) {
                break;
            }
            value.push_back(advance());
        }
        if (value.empty()) {
            value.push_back(advance());
        }
        return Token{.kind = TokenKind::Word,
                     .text = std::move(value),
                     .line = start_line,
                     .column = start_column};
    }

    std::string_view text_;
    std::size_t offset_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

class TokenStream {
public:
    explicit TokenStream(std::string_view text) : lexer_(text) { current_ = lexer_.next(); }

    const Token& current() const { return current_; }
    bool eof() const { return current_.kind == TokenKind::End; }

    Token take() {
        Token old = current_;
        current_ = lexer_.next();
        return old;
    }

    bool consume(TokenKind kind) {
        if (current_.kind != kind) {
            return false;
        }
        take();
        return true;
    }

    bool consumeWord(std::string_view word) {
        if (current_.kind != TokenKind::Word || !equalsIgnoreCase(current_.text, word)) {
            return false;
        }
        take();
        return true;
    }

private:
    Lexer lexer_;
    Token current_;
};

class ParserBase {
public:
    explicit ParserBase(std::string_view text, std::string_view file_name) :
        tokens_(text), file_name_(file_name) {}

    std::vector<LayoutDiagnostic> takeDiagnostics() { return std::move(diagnostics_); }

protected:
    bool eof() const { return tokens_.eof(); }
    const Token& current() const { return tokens_.current(); }
    Token take() { return tokens_.take(); }
    bool consume(TokenKind kind) { return tokens_.consume(kind); }
    bool consumeWord(std::string_view word) { return tokens_.consumeWord(word); }

    void warning(std::string message, const Token& token) {
        addDiagnostic(LayoutDiagnosticSeverity::Warning, std::move(message), token);
    }

    void error(std::string message, const Token& token) {
        addDiagnostic(LayoutDiagnosticSeverity::Error, std::move(message), token);
    }

    void addDiagnostic(LayoutDiagnosticSeverity severity, std::string message, const Token& token) {
        if (!file_name_.empty()) {
            message = std::string(file_name_) + ": " + message;
        }
        diagnostics_.push_back(LayoutDiagnostic{.severity = severity,
                                                .message = std::move(message),
                                                .line = token.line,
                                                .column = token.column});
    }

    std::optional<std::string> parseName() {
        if (current().kind == TokenKind::Word || current().kind == TokenKind::String ||
            current().kind == TokenKind::Number) {
            return take().text;
        }
        error("Expected name", current());
        return std::nullopt;
    }

    std::optional<double> parseNumber() {
        if (current().kind == TokenKind::Number) {
            return take().number;
        }
        error("Expected number", current());
        return std::nullopt;
    }

    std::optional<LayoutPoint> parsePoint(double scale) {
        if (!consume(TokenKind::LParen)) {
            error("Expected '('", current());
            return std::nullopt;
        }
        const auto x = parseNumber();
        const auto y = parseNumber();
        if (!consume(TokenKind::RParen)) {
            error("Expected ')'", current());
        }
        if (!x.has_value() || !y.has_value()) {
            return std::nullopt;
        }
        return LayoutPoint{.x = toDbu(*x, scale), .y = toDbu(*y, scale)};
    }

    static std::int64_t toDbu(double value, double scale) {
        return static_cast<std::int64_t>(std::llround(value * scale));
    }

    void skipUntilSemicolon() {
        while (!eof()) {
            if (consume(TokenKind::Semicolon)) {
                return;
            }
            take();
        }
    }

    void skipSection(std::string_view end_name) {
        while (!eof()) {
            if (consumeWord("END")) {
                if (current().kind == TokenKind::Word || current().kind == TokenKind::String ||
                    current().kind == TokenKind::Number) {
                    const auto name = take().text;
                    if (end_name.empty() || equalsIgnoreCase(name, end_name)) {
                        return;
                    }
                }
            }
            else {
                take();
            }
        }
    }

private:
    TokenStream tokens_;
    std::string file_name_;
    std::vector<LayoutDiagnostic> diagnostics_;
};

std::uint32_t findOrAddLayer(std::vector<LayoutLayer>& layers, std::string_view name) {
    for (std::size_t index = 0; index < layers.size(); ++index) {
        if (equalsIgnoreCase(layers[index].name, name)) {
            return static_cast<std::uint32_t>(index);
        }
    }
    layers.push_back(LayoutLayer{.name = std::string(name)});
    return static_cast<std::uint32_t>(layers.size() - 1U);
}

LayoutLayerKind parseLayerKind(std::string_view value) {
    if (equalsIgnoreCase(value, "ROUTING")) {
        return LayoutLayerKind::Routing;
    }
    if (equalsIgnoreCase(value, "CUT")) {
        return LayoutLayerKind::Cut;
    }
    if (equalsIgnoreCase(value, "IMPLANT")) {
        return LayoutLayerKind::Implant;
    }
    if (equalsIgnoreCase(value, "MASTERSLICE")) {
        return LayoutLayerKind::Masterslice;
    }
    if (equalsIgnoreCase(value, "OVERLAP")) {
        return LayoutLayerKind::Overlap;
    }
    return LayoutLayerKind::Unknown;
}

LayoutPinDirection parsePinDirection(std::string_view value) {
    if (equalsIgnoreCase(value, "INPUT")) {
        return LayoutPinDirection::Input;
    }
    if (equalsIgnoreCase(value, "OUTPUT")) {
        return LayoutPinDirection::Output;
    }
    if (equalsIgnoreCase(value, "INOUT")) {
        return LayoutPinDirection::Inout;
    }
    if (equalsIgnoreCase(value, "FEEDTHRU")) {
        return LayoutPinDirection::Feedthru;
    }
    return LayoutPinDirection::Unknown;
}

LayoutPlacementStatus parsePlacementStatus(std::string_view value) {
    if (equalsIgnoreCase(value, "PLACED")) {
        return LayoutPlacementStatus::Placed;
    }
    if (equalsIgnoreCase(value, "FIXED")) {
        return LayoutPlacementStatus::Fixed;
    }
    if (equalsIgnoreCase(value, "COVER")) {
        return LayoutPlacementStatus::Cover;
    }
    if (equalsIgnoreCase(value, "UNPLACED")) {
        return LayoutPlacementStatus::Unplaced;
    }
    return LayoutPlacementStatus::Unknown;
}

class LefParser final : public ParserBase {
public:
    LefParser(std::string_view text, std::string_view file_name) : ParserBase(text, file_name) {}

    ParseResult<LayoutLefLibrary> parse() {
        while (!eof()) {
            if (consume(TokenKind::Semicolon)) {
                continue;
            }
            if (consumeWord("VERSION")) {
                if (auto value = parseName()) {
                    library_.version = *value;
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("UNITS")) {
                parseUnits();
                continue;
            }
            if (consumeWord("MANUFACTURINGGRID")) {
                if (auto grid = parseNumber()) {
                    library_.manufacturing_grid = *grid;
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("LAYER")) {
                parseLayer();
                continue;
            }
            if (consumeWord("VIA")) {
                parseVia();
                continue;
            }
            if (consumeWord("SITE")) {
                parseSite();
                continue;
            }
            if (consumeWord("MACRO")) {
                parseMacro();
                continue;
            }
            if (consumeWord("END")) {
                if (!eof() && current().kind != TokenKind::Semicolon) {
                    take();
                }
                consume(TokenKind::Semicolon);
                continue;
            }

            const auto token = take();
            warning("Unsupported LEF statement '" + token.text + "'", token);
            skipUntilSemicolon();
        }
        auto diagnostics = takeDiagnostics();
        library_.diagnostics = diagnostics;
        return ParseResult<LayoutLefLibrary>{.value = std::move(library_),
                                             .diagnostics = std::move(diagnostics)};
    }

private:
    void parseUnits() {
        while (!eof()) {
            if (consume(TokenKind::Semicolon)) {
                return;
            }
            if (consumeWord("DATABASE")) {
                consumeWord("MICRONS");
                if (auto value = parseNumber()) {
                    library_.units_per_micron = static_cast<std::uint32_t>(std::llround(*value));
                }
                consume(TokenKind::Semicolon);
                return;
            }
            take();
        }
    }

    void parseLayer() {
        const auto name = parseName();
        if (!name.has_value()) {
            skipSection({});
            return;
        }
        auto layer = LayoutLayer{.name = *name};
        while (!eof()) {
            if (consumeWord("END")) {
                parseName();
                library_.layers.push_back(std::move(layer));
                return;
            }
            if (consumeWord("TYPE")) {
                if (auto value = parseName()) {
                    layer.kind = parseLayerKind(*value);
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("PITCH")) {
                layer.pitch = parseNumber().value_or(0.0);
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("WIDTH")) {
                layer.width = parseNumber().value_or(0.0);
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("SPACING")) {
                layer.spacing = parseNumber().value_or(0.0);
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consume(TokenKind::Semicolon)) {
                continue;
            }
            take();
            skipUntilSemicolon();
        }
        library_.layers.push_back(std::move(layer));
    }

    void parseVia() {
        const auto name = parseName();
        if (!name.has_value()) {
            skipSection({});
            return;
        }
        LayoutVia via{.name = *name};
        std::uint32_t active_layer = 0;
        while (!eof()) {
            if (consumeWord("END")) {
                parseName();
                library_.vias.push_back(std::move(via));
                return;
            }
            if (consumeWord("LAYER")) {
                if (auto layer_name = parseName()) {
                    active_layer = findOrAddLayer(library_.layers, *layer_name);
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("RECT")) {
                if (auto rect = parseRect(static_cast<double>(library_.units_per_micron))) {
                    via.shapes.push_back(LayoutViaShape{.layer_index = active_layer,
                                                        .rect = *rect});
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consume(TokenKind::Semicolon)) {
                continue;
            }
            take();
            skipUntilSemicolon();
        }
        library_.vias.push_back(std::move(via));
    }

    void parseSite() {
        const auto name = parseName();
        if (!name.has_value()) {
            skipSection({});
            return;
        }
        LayoutSite site{.name = *name};
        while (!eof()) {
            if (consumeWord("END")) {
                parseName();
                library_.sites.push_back(std::move(site));
                return;
            }
            if (consumeWord("SIZE")) {
                site.width = parseNumber().value_or(0.0);
                consumeWord("BY");
                site.height = parseNumber().value_or(0.0);
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consume(TokenKind::Semicolon)) {
                continue;
            }
            take();
            skipUntilSemicolon();
        }
        library_.sites.push_back(std::move(site));
    }

    void parseMacro() {
        const auto name = parseName();
        if (!name.has_value()) {
            skipSection({});
            return;
        }
        LayoutMacro macro{.name = *name};
        while (!eof()) {
            if (consumeWord("END")) {
                parseName();
                library_.macros.push_back(std::move(macro));
                return;
            }
            if (consumeWord("CLASS")) {
                std::string class_name;
                while (!eof() && current().kind != TokenKind::Semicolon) {
                    if (!class_name.empty()) {
                        class_name.push_back(' ');
                    }
                    class_name += take().text;
                }
                macro.class_name = std::move(class_name);
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("ORIGIN")) {
                macro.origin_x = parseNumber().value_or(0.0);
                macro.origin_y = parseNumber().value_or(0.0);
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("SIZE")) {
                macro.size_x = parseNumber().value_or(0.0);
                consumeWord("BY");
                macro.size_y = parseNumber().value_or(0.0);
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("PIN")) {
                parseMacroPin(macro);
                continue;
            }
            if (consumeWord("OBS")) {
                parseObs(macro);
                continue;
            }
            if (consume(TokenKind::Semicolon)) {
                continue;
            }
            take();
            skipUntilSemicolon();
        }
        library_.macros.push_back(std::move(macro));
    }

    void parseMacroPin(LayoutMacro& macro) {
        const auto name = parseName();
        if (!name.has_value()) {
            skipSection({});
            return;
        }
        LayoutPin pin{.name = *name};
        while (!eof()) {
            if (consumeWord("END")) {
                parseName();
                macro.pins.push_back(std::move(pin));
                return;
            }
            if (consumeWord("DIRECTION")) {
                if (auto value = parseName()) {
                    pin.direction = parsePinDirection(*value);
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("USE")) {
                if (auto value = parseName()) {
                    pin.use = *value;
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("PORT")) {
                pin.ports.push_back(parsePort(LayoutOwnerKind::Pin,
                                             static_cast<std::uint32_t>(macro.pins.size())));
                continue;
            }
            if (consume(TokenKind::Semicolon)) {
                continue;
            }
            take();
            skipUntilSemicolon();
        }
        macro.pins.push_back(std::move(pin));
    }

    LayoutPort parsePort(LayoutOwnerKind owner_kind, std::uint32_t owner_index) {
        LayoutPort port;
        std::uint32_t active_layer = 0;
        const auto scale = static_cast<double>(library_.units_per_micron);
        while (!eof()) {
            if (consumeWord("END")) {
                return port;
            }
            if (consumeWord("LAYER")) {
                if (auto layer_name = parseName()) {
                    active_layer = findOrAddLayer(library_.layers, *layer_name);
                }
                skipUntilSemicolon();
                continue;
            }
            if (consumeWord("RECT")) {
                if (auto rect = parseRect(scale)) {
                    port.shapes.push_back(LayoutShape{.kind = LayoutShapeKind::Rect,
                                                      .owner_kind = owner_kind,
                                                      .owner_index = owner_index,
                                                      .layer_index = active_layer,
                                                      .rect = *rect});
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("POLYGON")) {
                if (auto polygon = parsePolygon(scale)) {
                    port.shapes.push_back(LayoutShape{.kind = LayoutShapeKind::Polygon,
                                                      .owner_kind = owner_kind,
                                                      .owner_index = owner_index,
                                                      .layer_index = active_layer,
                                                      .polygon = *polygon});
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consume(TokenKind::Semicolon)) {
                continue;
            }
            take();
            skipUntilSemicolon();
        }
        return port;
    }

    void parseObs(LayoutMacro& macro) {
        std::uint32_t active_layer = 0;
        const auto scale = static_cast<double>(library_.units_per_micron);
        while (!eof()) {
            if (consumeWord("END")) {
                return;
            }
            if (consumeWord("LAYER")) {
                if (auto layer_name = parseName()) {
                    active_layer = findOrAddLayer(library_.layers, *layer_name);
                }
                skipUntilSemicolon();
                continue;
            }
            if (consumeWord("RECT")) {
                if (auto rect = parseRect(scale)) {
                    macro.obstructions.push_back(LayoutShape{.kind = LayoutShapeKind::Rect,
                                                            .owner_kind =
                                                                LayoutOwnerKind::Obstruction,
                                                            .owner_index = static_cast<std::uint32_t>(
                                                                macro.obstructions.size()),
                                                            .layer_index = active_layer,
                                                            .rect = *rect});
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("POLYGON")) {
                if (auto polygon = parsePolygon(scale)) {
                    macro.obstructions.push_back(LayoutShape{.kind = LayoutShapeKind::Polygon,
                                                            .owner_kind =
                                                                LayoutOwnerKind::Obstruction,
                                                            .owner_index = static_cast<std::uint32_t>(
                                                                macro.obstructions.size()),
                                                            .layer_index = active_layer,
                                                            .polygon = *polygon});
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consume(TokenKind::Semicolon)) {
                continue;
            }
            take();
            skipUntilSemicolon();
        }
    }

    std::optional<LayoutRect> parseRect(double scale) {
        const auto x0 = parseNumber();
        const auto y0 = parseNumber();
        const auto x1 = parseNumber();
        const auto y1 = parseNumber();
        if (!x0.has_value() || !y0.has_value() || !x1.has_value() || !y1.has_value()) {
            return std::nullopt;
        }
        return normalizedRect(toDbu(*x0, scale),
                              toDbu(*y0, scale),
                              toDbu(*x1, scale),
                              toDbu(*y1, scale));
    }

    std::optional<LayoutPolygon> parsePolygon(double scale) {
        LayoutPolygon polygon;
        while (!eof() && current().kind != TokenKind::Semicolon) {
            const auto x = parseNumber();
            const auto y = parseNumber();
            if (!x.has_value() || !y.has_value()) {
                break;
            }
            polygon.points.push_back(LayoutPoint{.x = toDbu(*x, scale),
                                                 .y = toDbu(*y, scale)});
        }
        if (polygon.points.size() < 3) {
            warning("POLYGON has fewer than three points", current());
        }
        return polygon;
    }

    static LayoutRect normalizedRect(std::int64_t x0,
                                     std::int64_t y0,
                                     std::int64_t x1,
                                     std::int64_t y1) {
        return LayoutRect{.x0 = std::min(x0, x1),
                          .y0 = std::min(y0, y1),
                          .x1 = std::max(x0, x1),
                          .y1 = std::max(y0, y1)};
    }

    LayoutLefLibrary library_;
};

class DefParser final : public ParserBase {
public:
    DefParser(std::string_view text, std::string_view file_name) : ParserBase(text, file_name) {}

    ParseResult<LayoutDefDesign> parse() {
        while (!eof()) {
            if (consume(TokenKind::Semicolon)) {
                continue;
            }
            if (consumeWord("VERSION")) {
                if (auto value = parseName()) {
                    design_.version = *value;
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("DESIGN")) {
                if (auto value = parseName()) {
                    design_.design_name = *value;
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            if (consumeWord("UNITS")) {
                parseUnits();
                continue;
            }
            if (consumeWord("DIEAREA")) {
                parseDieArea();
                continue;
            }
            if (consumeWord("COMPONENTS")) {
                parseComponents();
                continue;
            }
            if (consumeWord("PINS")) {
                parsePins();
                continue;
            }
            if (consumeWord("NETS")) {
                parseNets(false);
                continue;
            }
            if (consumeWord("SPECIALNETS")) {
                parseNets(true);
                continue;
            }
            if (consumeWord("BLOCKAGES")) {
                parseBlockages();
                continue;
            }
            if (consumeWord("END")) {
                if (!eof() && current().kind != TokenKind::Semicolon) {
                    take();
                }
                consume(TokenKind::Semicolon);
                continue;
            }
            const auto token = take();
            warning("Unsupported DEF statement '" + token.text + "'", token);
            skipUntilSemicolon();
        }
        auto diagnostics = takeDiagnostics();
        for (const auto& layer_name : def_layers_) {
            design_.layers.push_back(LayoutLayer{.name = layer_name});
        }
        design_.diagnostics = diagnostics;
        return ParseResult<LayoutDefDesign>{.value = std::move(design_),
                                            .diagnostics = std::move(diagnostics)};
    }

private:
    void parseUnits() {
        while (!eof()) {
            if (consume(TokenKind::Semicolon)) {
                return;
            }
            if (consumeWord("DISTANCE")) {
                consumeWord("MICRONS");
                if (auto value = parseNumber()) {
                    design_.units_per_micron = static_cast<std::uint32_t>(std::llround(*value));
                }
                consume(TokenKind::Semicolon);
                return;
            }
            take();
        }
    }

    void parseDieArea() {
        const auto first = parsePoint(1.0);
        const auto second = parsePoint(1.0);
        if (first.has_value() && second.has_value()) {
            design_.die_area = LayoutRect{.x0 = std::min(first->x, second->x),
                                          .y0 = std::min(first->y, second->y),
                                          .x1 = std::max(first->x, second->x),
                                          .y1 = std::max(first->y, second->y)};
        }
        consume(TokenKind::Semicolon);
    }

    void parseComponents() {
        skipSectionCount();
        while (!eof()) {
            if (consumeWord("END")) {
                consumeWord("COMPONENTS");
                return;
            }
            if (!consume(TokenKind::Minus)) {
                take();
                continue;
            }
            LayoutComponent component;
            if (auto name = parseName()) {
                component.name = *name;
            }
            if (auto macro = parseName()) {
                component.macro_name = *macro;
            }
            while (!eof() && current().kind != TokenKind::Semicolon) {
                if (consume(TokenKind::Plus)) {
                    if (auto keyword = parseName()) {
                        if (equalsIgnoreCase(*keyword, "PLACED") ||
                            equalsIgnoreCase(*keyword, "FIXED") ||
                            equalsIgnoreCase(*keyword, "COVER") ||
                            equalsIgnoreCase(*keyword, "UNPLACED")) {
                            component.status = parsePlacementStatus(*keyword);
                            if (auto point = parsePoint(1.0)) {
                                component.x = point->x;
                                component.y = point->y;
                            }
                            if (auto orient = parseName()) {
                                component.orientation = *orient;
                            }
                        }
                    }
                    continue;
                }
                take();
            }
            consume(TokenKind::Semicolon);
            design_.components.push_back(std::move(component));
        }
    }

    void parsePins() {
        skipSectionCount();
        while (!eof()) {
            if (consumeWord("END")) {
                consumeWord("PINS");
                return;
            }
            if (!consume(TokenKind::Minus)) {
                take();
                continue;
            }
            LayoutDefPin pin;
            if (auto name = parseName()) {
                pin.name = *name;
            }
            while (!eof() && current().kind != TokenKind::Semicolon) {
                if (consume(TokenKind::Plus)) {
                    if (auto keyword = parseName()) {
                        if (equalsIgnoreCase(*keyword, "NET")) {
                            if (auto net = parseName()) {
                                pin.net_name = *net;
                            }
                        }
                        else if (equalsIgnoreCase(*keyword, "PLACED") ||
                                 equalsIgnoreCase(*keyword, "FIXED") ||
                                 equalsIgnoreCase(*keyword, "COVER") ||
                                 equalsIgnoreCase(*keyword, "UNPLACED")) {
                            pin.status = parsePlacementStatus(*keyword);
                            if (auto point = parsePoint(1.0)) {
                                pin.x = point->x;
                                pin.y = point->y;
                            }
                            if (auto orient = parseName()) {
                                pin.orientation = *orient;
                            }
                        }
                        else if (equalsIgnoreCase(*keyword, "LAYER")) {
                            auto shape = parseDefLayerShape(LayoutOwnerKind::Pin,
                                                            static_cast<std::uint32_t>(
                                                                design_.pins.size()));
                            if (shape.has_value()) {
                                pin.shapes.push_back(std::move(*shape));
                            }
                        }
                    }
                    continue;
                }
                take();
            }
            consume(TokenKind::Semicolon);
            design_.pins.push_back(std::move(pin));
        }
    }

    void parseNets(bool special) {
        skipSectionCount();
        while (!eof()) {
            const auto section_name = special ? "SPECIALNETS" : "NETS";
            if (consumeWord("END")) {
                consumeWord(section_name);
                return;
            }
            if (!consume(TokenKind::Minus)) {
                take();
                continue;
            }
            LayoutNet net;
            net.special = special;
            if (auto name = parseName()) {
                net.name = *name;
            }
            while (!eof() && current().kind != TokenKind::Semicolon) {
                if (consume(TokenKind::LParen)) {
                    LayoutNetConnection connection;
                    if (auto instance = parseName()) {
                        connection.instance = *instance;
                    }
                    if (auto pin = parseName()) {
                        connection.pin = *pin;
                    }
                    consume(TokenKind::RParen);
                    net.connections.push_back(std::move(connection));
                    continue;
                }
                if (consume(TokenKind::Plus)) {
                    if (auto keyword = parseName()) {
                        if (equalsIgnoreCase(*keyword, "ROUTED") ||
                            equalsIgnoreCase(*keyword, "FIXED") ||
                            equalsIgnoreCase(*keyword, "COVER") ||
                            equalsIgnoreCase(*keyword, "SHIELD")) {
                            parseNetRoute(net, special);
                        }
                    }
                    continue;
                }
                take();
            }
            consume(TokenKind::Semicolon);
            design_.nets.push_back(std::move(net));
        }
    }

    void parseNetRoute(LayoutNet& net, bool special) {
        std::uint32_t layer_index = 0;
        if (auto layer = parseName()) {
            layer_index = findLayer(*layer);
        }
        std::vector<LayoutPoint> points;
        while (!eof() && current().kind != TokenKind::Semicolon && current().kind != TokenKind::Plus) {
            if (current().kind == TokenKind::LParen) {
                if (auto point = parsePoint(1.0)) {
                    points.push_back(*point);
                }
                continue;
            }
            take();
        }
        for (std::size_t index = 1; index < points.size(); ++index) {
            const auto& a = points[index - 1U];
            const auto& b = points[index];
            net.shapes.push_back(LayoutShape{.kind = LayoutShapeKind::Rect,
                                             .owner_kind = special
                                                 ? LayoutOwnerKind::SpecialNet
                                                 : LayoutOwnerKind::Net,
                                             .owner_index =
                                                 static_cast<std::uint32_t>(design_.nets.size()),
                                             .layer_index = layer_index,
                                             .rect = LayoutRect{.x0 = std::min(a.x, b.x),
                                                                .y0 = std::min(a.y, b.y),
                                                                .x1 = std::max(a.x, b.x),
                                                                .y1 = std::max(a.y, b.y)}});
        }
    }

    void parseBlockages() {
        skipSectionCount();
        while (!eof()) {
            if (consumeWord("END")) {
                consumeWord("BLOCKAGES");
                return;
            }
            if (!consume(TokenKind::Minus)) {
                take();
                continue;
            }
            while (!eof() && current().kind != TokenKind::Semicolon) {
                if (consume(TokenKind::Plus)) {
                    continue;
                }
                if (current().kind == TokenKind::Word && equalsIgnoreCase(current().text, "LAYER")) {
                    take();
                    auto shape = parseDefLayerShape(LayoutOwnerKind::Blockage,
                                                    static_cast<std::uint32_t>(
                                                        design_.blockages.size()));
                    if (shape.has_value()) {
                        design_.blockages.push_back(std::move(*shape));
                    }
                    continue;
                }
                if (current().kind == TokenKind::Word) {
                    if (auto keyword = parseName()) {
                        if (!equalsIgnoreCase(*keyword, "PLACEMENT") &&
                            !equalsIgnoreCase(*keyword, "COMPONENT")) {
                            continue;
                        }
                    }
                    continue;
                }
                take();
            }
            consume(TokenKind::Semicolon);
        }
    }

    std::optional<LayoutShape> parseDefLayerShape(LayoutOwnerKind owner_kind,
                                                  std::uint32_t owner_index) {
        std::uint32_t layer_index = 0;
        if (auto layer = parseName()) {
            layer_index = findLayer(*layer);
        }
        while (!eof() && current().kind != TokenKind::Semicolon && current().kind != TokenKind::Plus) {
            if (current().kind == TokenKind::LParen) {
                const auto first = parsePoint(1.0);
                const auto second = parsePoint(1.0);
                if (first.has_value() && second.has_value()) {
                    return LayoutShape{.kind = LayoutShapeKind::Rect,
                                       .owner_kind = owner_kind,
                                       .owner_index = owner_index,
                                       .layer_index = layer_index,
                                       .rect = LayoutRect{.x0 = std::min(first->x, second->x),
                                                          .y0 = std::min(first->y, second->y),
                                                          .x1 = std::max(first->x, second->x),
                                                          .y1 = std::max(first->y, second->y)}};
                }
                continue;
            }
            take();
        }
        return std::nullopt;
    }

    void skipSectionCount() {
        while (!eof() && current().kind != TokenKind::Semicolon) {
            take();
        }
        consume(TokenKind::Semicolon);
    }

    std::uint32_t findLayer(std::string_view name) {
        for (std::size_t index = 0; index < def_layers_.size(); ++index) {
            if (equalsIgnoreCase(def_layers_[index], name)) {
                return static_cast<std::uint32_t>(index);
            }
        }
        def_layers_.push_back(std::string(name));
        return static_cast<std::uint32_t>(def_layers_.size() - 1U);
    }

    LayoutDefDesign design_;
    std::vector<std::string> def_layers_;
};

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

ParseResult<LayoutLefLibrary> parseLef(std::string_view text, std::string_view file_name) {
    return LefParser(text, file_name).parse();
}

ParseResult<LayoutDefDesign> parseDef(std::string_view text, std::string_view file_name) {
    return DefParser(text, file_name).parse();
}

ParseResult<LayoutLefLibrary> parseLefFile(const std::filesystem::path& path) {
    return parseLef(readTextFile(path), path.generic_string());
}

ParseResult<LayoutDefDesign> parseDefFile(const std::filesystem::path& path) {
    return parseDef(readTextFile(path), path.generic_string());
}

} // namespace pristine::layout
