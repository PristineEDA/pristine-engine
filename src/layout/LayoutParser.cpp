#include "pristine/layout/LayoutParser.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::layout {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kDefaultGdsProgressRecordInterval = 32768U;

std::uint64_t elapsedMicros(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

void addMicros(std::uint64_t& target, Clock::time_point start) {
    const auto elapsed = elapsedMicros(start);
    target = std::numeric_limits<std::uint64_t>::max() - target < elapsed
        ? std::numeric_limits<std::uint64_t>::max()
        : target + elapsed;
}

void addScaledMicros(std::uint64_t& target, Clock::time_point start, std::uint64_t scale) {
    const auto elapsed = elapsedMicros(start);
    if (elapsed == 0U) {
        return;
    }
    const auto scaled = elapsed > std::numeric_limits<std::uint64_t>::max() / scale
        ? std::numeric_limits<std::uint64_t>::max()
        : elapsed * scale;
    target = std::numeric_limits<std::uint64_t>::max() - target < scaled
        ? std::numeric_limits<std::uint64_t>::max()
        : target + scaled;
}

bool detailedGdsFinalizeProbesEnabled() {
#if defined(_WIN32)
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, "PRISTINE_GDS_DETAILED_PARSE_PROBES") != 0 || raw == nullptr) {
        return false;
    }
    const std::string value(raw, size == 0 ? 0 : size - 1U);
    std::free(raw);
#else
    const auto* raw = std::getenv("PRISTINE_GDS_DETAILED_PARSE_PROBES");
    if (raw == nullptr) {
        return false;
    }
    const std::string value(raw);
#endif
    const std::string_view text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "on" ||
           text == "ON" || text == "yes" || text == "YES";
}

class ScopedMicros {
public:
    explicit ScopedMicros(std::uint64_t& target) : target_(&target), start_(Clock::now()) {}
    ScopedMicros(const ScopedMicros&) = delete;
    ScopedMicros& operator=(const ScopedMicros&) = delete;
    ~ScopedMicros() { addMicros(*target_, start_); }

private:
    std::uint64_t* target_ = nullptr;
    Clock::time_point start_;
};

class OptionalScopedMicros {
public:
    OptionalScopedMicros(bool enabled, std::uint64_t& target) :
        target_(enabled ? &target : nullptr),
        start_(enabled ? Clock::now() : Clock::time_point{}) {}
    OptionalScopedMicros(const OptionalScopedMicros&) = delete;
    OptionalScopedMicros& operator=(const OptionalScopedMicros&) = delete;
    ~OptionalScopedMicros() {
        if (target_ != nullptr) {
            addMicros(*target_, start_);
        }
    }

private:
    std::uint64_t* target_ = nullptr;
    Clock::time_point start_;
};

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
    std::string text{};
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

constexpr std::uint8_t kGdsNoData = 0x00;
constexpr std::uint8_t kGdsBitArray = 0x01;
constexpr std::uint8_t kGdsInt2 = 0x02;
constexpr std::uint8_t kGdsInt4 = 0x03;
constexpr std::uint8_t kGdsReal8 = 0x05;
constexpr std::uint8_t kGdsString = 0x06;

enum class GdsRecordType : std::uint8_t {
    Header = 0x00,
    BgnLib = 0x01,
    LibName = 0x02,
    Units = 0x03,
    EndLib = 0x04,
    BgnStr = 0x05,
    StrName = 0x06,
    EndStr = 0x07,
    Boundary = 0x08,
    Path = 0x09,
    Sref = 0x0a,
    Aref = 0x0b,
    Text = 0x0c,
    Layer = 0x0d,
    Datatype = 0x0e,
    Width = 0x0f,
    Xy = 0x10,
    EndEl = 0x11,
    Sname = 0x12,
    ColRow = 0x13,
    TextType = 0x16,
    Presentation = 0x17,
    String = 0x19,
    Strans = 0x1a,
    Mag = 0x1b,
    Angle = 0x1c,
};

struct GdsRecord {
    GdsRecordType type = GdsRecordType::Header;
    std::uint8_t data_type = kGdsNoData;
    std::size_t data_offset = 0;
    std::size_t data_size = 0;
    std::size_t offset = 0;
};

std::string gdsRecordName(GdsRecordType type) {
    switch (type) {
        case GdsRecordType::Header:
            return "HEADER";
        case GdsRecordType::BgnLib:
            return "BGNLIB";
        case GdsRecordType::LibName:
            return "LIBNAME";
        case GdsRecordType::Units:
            return "UNITS";
        case GdsRecordType::EndLib:
            return "ENDLIB";
        case GdsRecordType::BgnStr:
            return "BGNSTR";
        case GdsRecordType::StrName:
            return "STRNAME";
        case GdsRecordType::EndStr:
            return "ENDSTR";
        case GdsRecordType::Boundary:
            return "BOUNDARY";
        case GdsRecordType::Path:
            return "PATH";
        case GdsRecordType::Sref:
            return "SREF";
        case GdsRecordType::Aref:
            return "AREF";
        case GdsRecordType::Text:
            return "TEXT";
        case GdsRecordType::Layer:
            return "LAYER";
        case GdsRecordType::Datatype:
            return "DATATYPE";
        case GdsRecordType::Width:
            return "WIDTH";
        case GdsRecordType::Xy:
            return "XY";
        case GdsRecordType::EndEl:
            return "ENDEL";
        case GdsRecordType::Sname:
            return "SNAME";
        case GdsRecordType::ColRow:
            return "COLROW";
        case GdsRecordType::TextType:
            return "TEXTTYPE";
        case GdsRecordType::Presentation:
            return "PRESENTATION";
        case GdsRecordType::String:
            return "STRING";
        case GdsRecordType::Strans:
            return "STRANS";
        case GdsRecordType::Mag:
            return "MAG";
        case GdsRecordType::Angle:
            return "ANGLE";
    }
    return "UNKNOWN";
}

class GdsParseCancelled final : public std::runtime_error {
public:
    GdsParseCancelled() : std::runtime_error("GDS parse cancelled") {}
};

class GdsParser final {
public:
    GdsParser(const std::vector<std::uint8_t>& bytes,
              std::string_view file_name,
              LayoutGdsParseControl* control = nullptr) :
        bytes_(bytes), file_name_(file_name), control_(control) {
        library_.cells.reserve(std::min<std::size_t>(bytes_.size() / 4096U + 16U, 1'000'000U));
        library_.references.reserve(
            std::min<std::size_t>(bytes_.size() / 1024U + 16U, 1'000'000U));
        library_.elements.reserve(
            std::min<std::size_t>(bytes_.size() / 64U + 16U, 4'000'000U));
        library_.points.reserve(
            std::min<std::size_t>(bytes_.size() / 12U + 16U, 16'000'000U));
        library_.text_element_indices.reserve(
            std::min<std::size_t>(bytes_.size() / 2048U + 16U, 1'000'000U));
        library_.layer_samples.reserve(4096U);
    }

    ParseResult<LayoutGdsLibrary> parse() {
        try {
            publishProgress(LayoutSourcePhase::Records, true);
            {
                ScopedMicros records_metric(metrics_.record_micros);
                while (offset_ < bytes_.size()) {
                    maybeCheckCancelled();
                    const auto record = nextRecord();
                    handleRecord(record);
                    maybePublishRecordProgress();
                }
            }
            publishProgress(LayoutSourcePhase::Finalize, true);
            if (current_cell_.has_value()) {
                warning("GDS ended before ENDSTR for cell '" + library_.cells[*current_cell_].name + "'",
                        bytes_.size());
                current_cell_.reset();
            }
            if (current_element_.has_value()) {
                warning("GDS ended before ENDEL for an element", bytes_.size());
                finishElement(bytes_.size());
            }
        }
        catch (const GdsParseCancelled&) {
            throw;
        }
        catch (const std::exception& error) {
            addDiagnostic(LayoutDiagnosticSeverity::Error, error.what(), offset_);
        }

        checkCancelled();
        flushUnsupportedRecordDiagnostics();

        {
            publishProgress(LayoutSourcePhase::Resolve, true);
            checkCancelled();
            ScopedMicros metric(metrics_.resolve_micros);
            resolveReferences();
            checkCancelled();
            inferTopCell();
        }
        metrics_.cell_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(library_.cells.size(), std::numeric_limits<std::uint32_t>::max()));
        metrics_.reference_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(library_.references.size(), std::numeric_limits<std::uint32_t>::max()));
        metrics_.element_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(library_.elements.size(), std::numeric_limits<std::uint32_t>::max()));
        if (metrics_.element_count > 0U && metrics_.element_finalize_micros == 0U) {
            metrics_.element_finalize_micros = 1U;
        }
        library_.parse_metrics = metrics_;
        library_.diagnostics.insert(library_.diagnostics.end(), diagnostics_.begin(), diagnostics_.end());
        return ParseResult<LayoutGdsLibrary>{.value = std::move(library_),
                                             .diagnostics = std::move(diagnostics_)};
    }

private:
    void checkCancelled() {
        if (control_ == nullptr || !control_->should_cancel) {
            return;
        }
        if (metrics_.cancel_check_count != std::numeric_limits<std::uint32_t>::max()) {
            ++metrics_.cancel_check_count;
        }
        if (control_->should_cancel()) {
            throw GdsParseCancelled();
        }
    }

    void maybeCheckCancelled() {
        if (control_ == nullptr || !control_->should_cancel) {
            return;
        }
        const auto interval = control_->progress_record_interval == 0U
                                  ? kDefaultGdsProgressRecordInterval
                                  : control_->progress_record_interval;
        if (metrics_.record_count == 0U || (metrics_.record_count % interval) == 0U) {
            checkCancelled();
        }
    }

    void maybePublishRecordProgress() {
        if (control_ == nullptr || !control_->publish_progress) {
            return;
        }
        const auto interval = control_->progress_record_interval == 0U
                                  ? kDefaultGdsProgressRecordInterval
                                  : control_->progress_record_interval;
        if (metrics_.record_count == last_progress_record_count_ ||
            (metrics_.record_count % interval) != 0U) {
            return;
        }
        last_progress_record_count_ = metrics_.record_count;
        publishProgress(LayoutSourcePhase::Records, false);
    }

    void publishProgress(LayoutSourcePhase phase, bool force) {
        if (control_ == nullptr || !control_->publish_progress) {
            return;
        }
        if (!force && metrics_.record_count == last_progress_record_count_) {
            return;
        }
        LayoutGdsParseProgress progress;
        progress.phase = phase;
        progress.file_size_bytes = bytes_.size();
        progress.bytes_read = offset_;
        progress.record_count = metrics_.record_count;
        progress.cell_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(library_.cells.size(), std::numeric_limits<std::uint32_t>::max()));
        progress.reference_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(library_.references.size(), std::numeric_limits<std::uint32_t>::max()));
        progress.element_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(library_.elements.size(), std::numeric_limits<std::uint32_t>::max()));
        progress.point_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(library_.points.size(), std::numeric_limits<std::uint32_t>::max()));
        progress.string_count = metrics_.string_count;
        progress.diagnostic_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(diagnostics_.size(), std::numeric_limits<std::uint32_t>::max()));
        progress.suppressed_diagnostic_count = metrics_.suppressed_diagnostic_count;
        progress.arena_growth_count = metrics_.arena_growth_count;
        progress.cancel_check_count = metrics_.cancel_check_count;
        control_->publish_progress(progress);
    }

    GdsRecord nextRecord() {
        if (bytes_.size() - offset_ < 4U) {
            throw std::runtime_error("Truncated GDS record header");
        }
        OptionalScopedMicros metric(detailed_finalize_probes_, metrics_.record_micros);
        const auto record_offset = offset_;
        const auto length = readU16(record_offset);
        if (length < 4U) {
            throw std::runtime_error("Invalid GDS record length");
        }
        if (length > bytes_.size() - offset_) {
            throw std::runtime_error("Truncated GDS record payload");
        }
        const auto record_type = static_cast<GdsRecordType>(bytes_[offset_ + 2U]);
        const auto data_type = bytes_[offset_ + 3U];
        offset_ += length;
        if (metrics_.record_count != std::numeric_limits<std::uint32_t>::max()) {
            ++metrics_.record_count;
        }
        return GdsRecord{.type = record_type,
                         .data_type = data_type,
                         .data_offset = record_offset + 4U,
                         .data_size = length - 4U,
                         .offset = record_offset};
    }

    void handleRecord(const GdsRecord& record) {
        switch (record.type) {
            case GdsRecordType::Header:
                if (auto version = firstInt2(record); version.has_value()) {
                    library_.version = static_cast<std::uint16_t>(*version);
                }
                return;
            case GdsRecordType::LibName:
                library_.name = stringValue(record);
                return;
            case GdsRecordType::Units:
                parseUnits(record);
                return;
            case GdsRecordType::BgnStr:
                startCell(record.offset);
                return;
            case GdsRecordType::StrName:
                setCurrentCellName(stringValue(record), record.offset);
                return;
            case GdsRecordType::EndStr:
                current_cell_.reset();
                return;
            case GdsRecordType::Boundary:
                startElement(LayoutGdsElementKind::Boundary, record.offset);
                return;
            case GdsRecordType::Path:
                startElement(LayoutGdsElementKind::Path, record.offset);
                return;
            case GdsRecordType::Sref:
                startElement(LayoutGdsElementKind::Sref, record.offset);
                return;
            case GdsRecordType::Aref:
                startElement(LayoutGdsElementKind::Aref, record.offset);
                return;
            case GdsRecordType::Text:
                startElement(LayoutGdsElementKind::Text, record.offset);
                return;
            case GdsRecordType::Layer:
                if (auto layer = firstInt2(record); layer.has_value()) {
                    if (current_element_.has_value()) {
                        current_element_->layer = *layer;
                    }
                }
                return;
            case GdsRecordType::Datatype:
                if (auto datatype = firstInt2(record); datatype.has_value()) {
                    if (current_element_.has_value()) {
                        current_element_->datatype = *datatype;
                    }
                }
                return;
            case GdsRecordType::TextType:
                if (auto texttype = firstInt2(record); texttype.has_value()) {
                    if (current_element_.has_value()) {
                        current_element_->texttype = *texttype;
                    }
                }
                return;
            case GdsRecordType::Xy:
                parseXy(record);
                return;
            case GdsRecordType::Sname:
            {
                auto name = stringValue(record);
                if (!current_reference_.has_value()) {
                    return;
                }
                current_reference_->target_name = std::move(name);
                return;
            }
            case GdsRecordType::ColRow:
                parseColRow(record);
                return;
            case GdsRecordType::Strans:
                parseStrans(record);
                return;
            case GdsRecordType::Mag:
            {
                const auto magnification = real8At(record, 0).value_or(1.0);
                if (!current_reference_.has_value()) {
                    return;
                }
                current_reference_->transform.magnification = magnification;
                return;
            }
            case GdsRecordType::Angle:
            {
                const auto angle = real8At(record, 0).value_or(0.0);
                if (!current_reference_.has_value()) {
                    return;
                }
                current_reference_->transform.angle = angle;
                return;
            }
            case GdsRecordType::String:
            {
                auto text = stringValue(record);
                if (!current_element_.has_value()) {
                    return;
                }
                current_element_->text = std::move(text);
                return;
            }
            case GdsRecordType::EndEl:
                finishElement(record.offset);
                return;
            case GdsRecordType::BgnLib:
            case GdsRecordType::EndLib:
            case GdsRecordType::Width:
            case GdsRecordType::Presentation:
                return;
        }
        unsupportedRecordWarning(record);
    }

    void parseUnits(const GdsRecord& record) {
        OptionalScopedMicros metric(detailed_finalize_probes_, metrics_.scalar_decode_micros);
        checkType(record, kGdsReal8);
        if (record.data_size % 8U != 0U) {
            warning("GDS " + gdsRecordName(record.type) + " has a truncated 8-byte real", record.offset);
        }
        if (record.data_size < 16U) {
            warning("GDS UNITS record is missing values", record.offset);
            return;
        }
        library_.user_unit_meters = readReal8(record.data_offset);
        library_.database_unit_meters = readReal8(record.data_offset + 8U);
        if (library_.database_unit_meters > 0.0) {
            const auto units = std::llround(1.0e-6 / library_.database_unit_meters);
            if (units > 0 && units <= std::numeric_limits<std::uint32_t>::max()) {
                library_.units_per_micron = static_cast<std::uint32_t>(units);
            }
        }
    }

    void startCell(std::size_t record_offset) {
        if (current_cell_.has_value()) {
            warning("Starting a GDS cell before ENDSTR closed the previous cell", record_offset);
        }
        library_.cells.push_back(LayoutGdsCell{});
        current_cell_ = static_cast<std::uint32_t>(library_.cells.size() - 1U);
    }

    void setCurrentCellName(std::string name, std::size_t record_offset) {
        if (!current_cell_.has_value()) {
            warning("STRNAME appeared outside a GDS cell", record_offset);
            return;
        }
        library_.cells[*current_cell_].name = std::move(name);
    }

    void startElement(LayoutGdsElementKind kind, std::size_t record_offset) {
        if (!current_cell_.has_value()) {
            warning("GDS element appeared outside a cell", record_offset);
            return;
        }
        if (current_element_.has_value()) {
            warning("Starting a GDS element before ENDEL closed the previous element", record_offset);
            finishElement(record_offset);
        }
        current_element_ = LayoutGdsElement{.kind = kind, .cell_index = *current_cell_};
        if (kind == LayoutGdsElementKind::Sref || kind == LayoutGdsElementKind::Aref) {
            current_reference_ = LayoutGdsReference{.kind = kind, .parent_cell_index = *current_cell_};
        }
    }

    void finishElement(std::size_t record_offset) {
        if (detailed_finalize_probes_) {
            finishElementWithDetailedProbes(record_offset);
            return;
        }
        if (!current_element_.has_value()) {
            warning("ENDEL appeared without an active GDS element", record_offset);
            return;
        }
        constexpr std::uint64_t kFinalizeProbeSampleRate = 1024U;
        const auto element_index_before_finalize = library_.elements.size();
        const auto sample_finalize_probe =
            (element_index_before_finalize % kFinalizeProbeSampleRate) == 0U;
        const auto finalize_sample_start =
            sample_finalize_probe ? Clock::now() : Clock::time_point{};
        auto addSampledMicros = [&](std::uint64_t& target, Clock::time_point start) {
            if (!sample_finalize_probe) {
                return;
            }
            addScaledMicros(target, start, kFinalizeProbeSampleRate);
        };
        auto element = std::move(*current_element_);
        current_element_.reset();
        const auto points = elementPoints(element);
        if (!element.bounds.has_value()) {
            const auto bbox_sample_start = sample_finalize_probe ? Clock::now() : Clock::time_point{};
            element.bounds = pointBounds(points);
            addSampledMicros(metrics_.bbox_micros, bbox_sample_start);
        }
        if (current_reference_.has_value()) {
            auto reference = std::move(*current_reference_);
            current_reference_.reset();
            if (!points.empty()) {
                reference.origin = points.front();
                if (reference.kind == LayoutGdsElementKind::Aref && points.size() >= 3U) {
                    reference.column_vector =
                        LayoutPoint{.x = points[1].x - points[0].x,
                                    .y = points[1].y - points[0].y};
                    reference.row_vector =
                        LayoutPoint{.x = points[2].x - points[0].x,
                                    .y = points[2].y - points[0].y};
                }
            }
            const auto ref_index = static_cast<std::uint32_t>(library_.references.size());
            element.reference_index = ref_index;
            library_.references.push_back(std::move(reference));
            if (element.cell_index < library_.cells.size()) {
                library_.cells[element.cell_index].reference_indices.push_back(ref_index);
            }
        }
        const auto element_index = static_cast<std::uint32_t>(library_.elements.size());
        if (element.cell_index < library_.cells.size()) {
            auto& cell = library_.cells[element.cell_index];
            cell.element_indices.push_back(element_index);
            expandCellBounds(cell.bounds, element.bounds);
        }
        const auto drawable = element.kind != LayoutGdsElementKind::Sref &&
                              element.kind != LayoutGdsElementKind::Aref && !points.empty();
        if (drawable) {
            const auto key = std::make_pair(element.layer, element.datatype);
            if (seen_layer_samples_.insert(key).second) {
                library_.layer_samples.push_back(LayoutGdsLayerSample{.layer = element.layer,
                                                                      .datatype = element.datatype,
                                                                      .element_index = element_index});
            }
            if (element.kind == LayoutGdsElementKind::Text) {
                library_.text_element_indices.push_back(element_index);
            }
        }
        library_.elements.push_back(std::move(element));
        addSampledMicros(metrics_.element_finalize_micros, finalize_sample_start);
    }

    void finishElementWithDetailedProbes(std::size_t record_offset) {
        if (!current_element_.has_value()) {
            warning("ENDEL appeared without an active GDS element", record_offset);
            return;
        }
        ScopedMicros finalize_metric(metrics_.element_finalize_micros);
        constexpr std::uint64_t kFinalizeProbeSampleRate = 1024U;
        const auto element_index_before_finalize = library_.elements.size();
        const auto sample_finalize_probe =
            detailed_finalize_probes_ &&
            (element_index_before_finalize % kFinalizeProbeSampleRate) == 0U;
        auto addSampledFinalizeMicros = [&](std::uint64_t& target, Clock::time_point start) {
            if (!sample_finalize_probe) {
                return;
            }
            addScaledMicros(target, start, kFinalizeProbeSampleRate);
        };
        auto element = std::move(*current_element_);
        current_element_.reset();
        const auto points = elementPoints(element);
        if (!element.bounds.has_value()) {
            ScopedMicros bbox_metric(metrics_.bbox_micros);
            const auto sample_start = sample_finalize_probe ? Clock::now() : Clock::time_point{};
            element.bounds = elementBounds(element);
            addSampledFinalizeMicros(metrics_.element_finalize_bbox_micros, sample_start);
        }
        const auto drawable = element.kind != LayoutGdsElementKind::Sref &&
                              element.kind != LayoutGdsElementKind::Aref && !points.empty();
        if (current_reference_.has_value()) {
            const auto sample_start = sample_finalize_probe ? Clock::now() : Clock::time_point{};
            auto reference = std::move(*current_reference_);
            current_reference_.reset();
            if (!points.empty()) {
                reference.origin = points.front();
                if (reference.kind == LayoutGdsElementKind::Aref && points.size() >= 3U) {
                    reference.column_vector =
                        LayoutPoint{.x = points[1].x - points[0].x,
                                    .y = points[1].y - points[0].y};
                    reference.row_vector =
                        LayoutPoint{.x = points[2].x - points[0].x,
                                    .y = points[2].y - points[0].y};
                }
            }
            const auto ref_index = static_cast<std::uint32_t>(library_.references.size());
            element.reference_index = ref_index;
            library_.references.push_back(std::move(reference));
            if (element.cell_index < library_.cells.size()) {
                library_.cells[element.cell_index].reference_indices.push_back(ref_index);
            }
            addSampledFinalizeMicros(metrics_.element_finalize_reference_micros, sample_start);
        }
        const auto element_index = static_cast<std::uint32_t>(library_.elements.size());
        {
            const auto sample_start = sample_finalize_probe ? Clock::now() : Clock::time_point{};
            if (element.cell_index < library_.cells.size()) {
                auto& cell = library_.cells[element.cell_index];
                cell.element_indices.push_back(element_index);
                expandCellBounds(cell.bounds, element.bounds);
            }
            addSampledFinalizeMicros(metrics_.element_finalize_index_micros, sample_start);
        }
        if (drawable) {
            const auto sample_start = sample_finalize_probe ? Clock::now() : Clock::time_point{};
            const auto key = std::make_pair(element.layer, element.datatype);
            if (seen_layer_samples_.insert(key).second) {
                library_.layer_samples.push_back(LayoutGdsLayerSample{.layer = element.layer,
                                                                      .datatype = element.datatype,
                                                                      .element_index = element_index});
            }
            if (element.kind == LayoutGdsElementKind::Text) {
                library_.text_element_indices.push_back(element_index);
            }
            addSampledFinalizeMicros(metrics_.element_finalize_sample_micros, sample_start);
        }
        library_.elements.push_back(std::move(element));
    }

    void parseXy(const GdsRecord& record) {
        OptionalScopedMicros metric(detailed_finalize_probes_, metrics_.xy_decode_micros);
        checkType(record, kGdsInt4);
        if (record.data_size % 4U != 0U) {
            warning("GDS " + gdsRecordName(record.type) + " has a truncated 4-byte integer", record.offset);
        }
        const auto value_count = record.data_size / 4U;
        if (value_count % 2U != 0U) {
            warning("GDS XY record has an odd coordinate count", record.offset);
        }
        if (!current_element_.has_value()) {
            return;
        }
        auto& element = *current_element_;
        const auto point_count = value_count / 2U;
        element.points.clear();
        element.bounds.reset();
        element.first_point = static_cast<std::uint32_t>(
            std::min<std::size_t>(library_.points.size(), std::numeric_limits<std::uint32_t>::max()));
        element.point_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(point_count, std::numeric_limits<std::uint32_t>::max()));
        if (library_.points.size() + point_count > std::numeric_limits<std::uint32_t>::max()) {
            warning("GDS point arena exceeds uint32 range", record.offset);
            element.first_point = 0;
            element.point_count = 0;
            return;
        }
        const auto needed_capacity = library_.points.size() + point_count;
        if (needed_capacity > library_.points.capacity()) {
            if (metrics_.arena_growth_count != std::numeric_limits<std::uint32_t>::max()) {
                ++metrics_.arena_growth_count;
            }
            const auto doubled = library_.points.capacity() > 0U
                ? library_.points.capacity() * 2U
                : needed_capacity;
            library_.points.reserve(std::max(needed_capacity, doubled));
        }
        constexpr std::uint64_t kXyBoundsProbeSampleRate = 1024U;
        const auto sample_bounds_probe =
            (library_.elements.size() % kXyBoundsProbeSampleRate) == 0U;
        const auto bounds_sample_start =
            sample_bounds_probe ? Clock::now() : Clock::time_point{};
        for (std::size_t index = 0; index < point_count; ++index) {
            const auto offset = record.data_offset + index * 8U;
            const LayoutPoint point{.x = readI32(offset), .y = readI32(offset + 4U)};
            if (index == 0U) {
                element.bounds = LayoutRect{.x0 = point.x,
                                            .y0 = point.y,
                                            .x1 = point.x,
                                            .y1 = point.y};
            }
            else if (element.bounds.has_value()) {
                element.bounds->x0 = std::min(element.bounds->x0, point.x);
                element.bounds->y0 = std::min(element.bounds->y0, point.y);
                element.bounds->x1 = std::max(element.bounds->x1, point.x);
                element.bounds->y1 = std::max(element.bounds->y1, point.y);
            }
            library_.points.push_back(point);
            if (metrics_.xy_point_count != std::numeric_limits<std::uint32_t>::max()) {
                ++metrics_.xy_point_count;
            }
        }
        if (sample_bounds_probe) {
            addScaledMicros(metrics_.xy_bounds_micros, bounds_sample_start,
                            kXyBoundsProbeSampleRate);
        }
    }

    void parseColRow(const GdsRecord& record) {
        OptionalScopedMicros metric(detailed_finalize_probes_, metrics_.scalar_decode_micros);
        checkType(record, kGdsInt2);
        if (record.data_size % 2U != 0U) {
            warning("GDS " + gdsRecordName(record.type) + " has a truncated 2-byte integer", record.offset);
        }
        if (record.data_size < 4U) {
            warning("GDS COLROW record is missing values", record.offset);
            return;
        }
        const auto columns = readU16(record.data_offset);
        const auto rows = readU16(record.data_offset + 2U);
        if (!current_reference_.has_value()) {
            return;
        }
        current_reference_->columns = columns == 0U ? 1U : columns;
        current_reference_->rows = rows == 0U ? 1U : rows;
    }

    void parseStrans(const GdsRecord& record) {
        OptionalScopedMicros metric(detailed_finalize_probes_, metrics_.scalar_decode_micros);
        checkType(record, kGdsBitArray);
        if (record.data_size % 2U != 0U) {
            warning("GDS " + gdsRecordName(record.type) + " has a truncated 2-byte integer", record.offset);
        }
        if (record.data_size < 2U) {
            warning("GDS STRANS record is missing flags", record.offset);
            return;
        }
        const auto reflected = (readU16(record.data_offset) & 0x8000U) != 0U;
        if (!current_reference_.has_value()) {
            return;
        }
        current_reference_->transform.reflected = reflected;
    }

    std::optional<std::uint32_t> firstInt2(const GdsRecord& record) {
        OptionalScopedMicros metric(detailed_finalize_probes_, metrics_.scalar_decode_micros);
        checkType(record, kGdsInt2);
        if (record.data_size % 2U != 0U) {
            warning("GDS " + gdsRecordName(record.type) + " has a truncated 2-byte integer", record.offset);
        }
        if (record.data_size < 2U) {
            warning("GDS " + gdsRecordName(record.type) + " is missing an integer value", record.offset);
            return std::nullopt;
        }
        return readU16(record.data_offset);
    }

    std::optional<double> real8At(const GdsRecord& record, std::size_t index) {
        OptionalScopedMicros metric(detailed_finalize_probes_, metrics_.scalar_decode_micros);
        checkType(record, kGdsReal8);
        if (record.data_size % 8U != 0U) {
            warning("GDS " + gdsRecordName(record.type) + " has a truncated 8-byte real", record.offset);
        }
        if (record.data_size < (index + 1U) * 8U) {
            warning("GDS " + gdsRecordName(record.type) + " is missing a real value", record.offset);
            return std::nullopt;
        }
        return readReal8(record.data_offset + index * 8U);
    }

    std::string stringValue(const GdsRecord& record) {
        OptionalScopedMicros metric(detailed_finalize_probes_, metrics_.string_decode_micros);
        checkType(record, kGdsString);
        if (metrics_.string_count != std::numeric_limits<std::uint32_t>::max()) {
            ++metrics_.string_count;
        }
        const auto end = record.data_offset + record.data_size;
        bool needs_cleanup = false;
        if (record.data_size > 0U && bytes_[end - 1U] == ' ') {
            needs_cleanup = true;
        }
        if (!needs_cleanup) {
            for (std::size_t offset = record.data_offset; offset < end; ++offset) {
                if (bytes_[offset] == 0) {
                    needs_cleanup = true;
                    break;
                }
            }
        }
        if (!needs_cleanup) {
            return std::string(reinterpret_cast<const char*>(bytes_.data() + record.data_offset),
                               record.data_size);
        }
        std::string value;
        value.reserve(record.data_size);
        for (std::size_t offset = record.data_offset; offset < end; ++offset) {
            if (bytes_[offset] != 0) {
                value.push_back(static_cast<char>(bytes_[offset]));
            }
        }
        while (!value.empty() && value.back() == ' ') {
            value.pop_back();
        }
        return value;
    }

    void checkType(const GdsRecord& record, std::uint8_t expected) {
        if (record.data_type != expected) {
            warning("GDS " + gdsRecordName(record.type) + " has unexpected data type", record.offset);
        }
    }

    std::uint16_t readU16(std::size_t offset) const {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes_[offset]) << 8U) |
                                          static_cast<std::uint16_t>(bytes_[offset + 1U]));
    }

    std::int32_t readI32(std::size_t offset) const {
        const std::uint32_t value = (static_cast<std::uint32_t>(bytes_[offset]) << 24U) |
                                    (static_cast<std::uint32_t>(bytes_[offset + 1U]) << 16U) |
                                    (static_cast<std::uint32_t>(bytes_[offset + 2U]) << 8U) |
                                    static_cast<std::uint32_t>(bytes_[offset + 3U]);
        return static_cast<std::int32_t>(value);
    }

    double readReal8(std::size_t offset) const {
        const auto first = bytes_[offset];
        if (first == 0U) {
            return 0.0;
        }
        const auto sign = (first & 0x80U) != 0U ? -1.0 : 1.0;
        const auto exponent = static_cast<int>(first & 0x7fU) - 64;
        std::uint64_t mantissa = 0;
        for (std::size_t index = 1; index < 8; ++index) {
            mantissa = (mantissa << 8U) | bytes_[offset + index];
        }
        const auto fraction = static_cast<double>(mantissa) / static_cast<double>(1ULL << 56U);
        return sign * fraction * std::pow(16.0, exponent);
    }

    void resolveReferences() {
        std::unordered_map<std::string_view, std::uint32_t> cell_by_name;
        {
            ScopedMicros lookup_metric(metrics_.resolve_lookup_micros);
            cell_by_name.reserve(library_.cells.size());
            for (std::size_t index = 0; index < library_.cells.size(); ++index) {
                const auto& name = library_.cells[index].name;
                if (name.empty()) {
                    warning("GDS cell has no STRNAME", 0);
                    continue;
                }
                cell_by_name.insert_or_assign(std::string_view(name),
                                              static_cast<std::uint32_t>(index));
            }
        }

        {
            ScopedMicros reference_metric(metrics_.resolve_reference_micros);
            for (auto& reference : library_.references) {
                const auto found = cell_by_name.find(std::string_view(reference.target_name));
                if (found == cell_by_name.end()) {
                    warning("GDS reference target '" + reference.target_name + "' is missing", 0);
                    continue;
                }
                reference.target_cell_index = found->second;
            }
        }
    }

    void inferTopCell() {
        ScopedMicros top_metric(metrics_.resolve_top_cell_micros);
        std::vector<bool> referenced(library_.cells.size(), false);
        for (const auto& reference : library_.references) {
            if (reference.target_cell_index < referenced.size()) {
                referenced[reference.target_cell_index] = true;
            }
        }
        for (std::size_t index = 0; index < library_.cells.size(); ++index) {
            if (!referenced[index]) {
                library_.top_cell_index = static_cast<std::uint32_t>(index);
                library_.cells[index].is_top = true;
                return;
            }
        }
        if (!library_.cells.empty()) {
            library_.top_cell_index = 0;
            library_.cells[0].is_top = true;
            warning("GDS hierarchy has no unreferenced top cell; possible reference cycle", 0);
        }
    }

    static void expandCellBounds(std::optional<LayoutRect>& bounds,
                                 const std::optional<LayoutRect>& rect) {
        if (!rect.has_value()) {
            return;
        }
        if (!bounds.has_value()) {
            bounds = *rect;
            return;
        }
        bounds->x0 = std::min(bounds->x0, rect->x0);
        bounds->y0 = std::min(bounds->y0, rect->y0);
        bounds->x1 = std::max(bounds->x1, rect->x1);
        bounds->y1 = std::max(bounds->y1, rect->y1);
    }

    std::span<const LayoutPoint> elementPoints(const LayoutGdsElement& element) const {
        if (element.point_count > 0U && element.first_point < library_.points.size()) {
            const auto available = library_.points.size() - element.first_point;
            const auto count = std::min<std::size_t>(element.point_count, available);
            return std::span<const LayoutPoint>(library_.points.data() + element.first_point, count);
        }
        return std::span<const LayoutPoint>(element.points.data(), element.points.size());
    }

    std::optional<LayoutRect> elementBounds(const LayoutGdsElement& element) const {
        const auto points = elementPoints(element);
        return pointBounds(points);
    }

    static std::optional<LayoutRect> pointBounds(std::span<const LayoutPoint> points) {
        if (points.empty()) {
            return std::nullopt;
        }
        LayoutRect bounds{.x0 = points.front().x,
                          .y0 = points.front().y,
                          .x1 = points.front().x,
                          .y1 = points.front().y};
        for (const auto& point : points) {
            bounds.x0 = std::min(bounds.x0, point.x);
            bounds.y0 = std::min(bounds.y0, point.y);
            bounds.x1 = std::max(bounds.x1, point.x);
            bounds.y1 = std::max(bounds.y1, point.y);
        }
        return bounds;
    }

    bool isDrawableElement(const LayoutGdsElement& element) const {
        return element.kind != LayoutGdsElementKind::Sref &&
               element.kind != LayoutGdsElementKind::Aref && !elementPoints(element).empty();
    }

    void warning(std::string message, std::size_t offset) {
        addDiagnostic(LayoutDiagnosticSeverity::Warning, std::move(message), offset);
    }

    void addDiagnostic(LayoutDiagnosticSeverity severity, std::string message, std::size_t offset) {
        ScopedMicros metric(metrics_.diagnostic_micros);
        if (!file_name_.empty()) {
            message = std::string(file_name_) + ": " + message;
        }
        diagnostics_.push_back(LayoutDiagnostic{.severity = severity,
                                                .message = std::move(message),
                                                .line = 1,
                                                .column = offset + 1U});
    }

    void unsupportedRecordWarning(const GdsRecord& record) {
        auto& aggregate = unsupported_records_[record.type];
        ++aggregate.count;
        if (aggregate.sample_offsets.size() < kUnsupportedRecordSampleLimit) {
            aggregate.sample_offsets.push_back(record.offset);
            warning("Skipping unsupported GDS record " + gdsRecordName(record.type), record.offset);
        }
    }

    void flushUnsupportedRecordDiagnostics() {
        for (const auto& [record_type, aggregate] : unsupported_records_) {
            if (aggregate.count <= aggregate.sample_offsets.size()) {
                continue;
            }
            const auto suppressed = aggregate.count - aggregate.sample_offsets.size();
            const auto available = std::numeric_limits<std::uint32_t>::max() -
                                   metrics_.suppressed_diagnostic_count;
            metrics_.suppressed_diagnostic_count += static_cast<std::uint32_t>(
                std::min<std::size_t>(suppressed, available));
            const auto offset = aggregate.sample_offsets.empty() ? 0U : aggregate.sample_offsets.front();
            warning("Suppressed " + std::to_string(suppressed) +
                        " additional unsupported GDS record " + gdsRecordName(record_type) +
                        " diagnostics",
                    offset);
        }
    }

    const std::vector<std::uint8_t>& bytes_;
    std::string file_name_;
    std::size_t offset_ = 0;
    LayoutGdsParseControl* control_ = nullptr;
    std::uint32_t last_progress_record_count_ = std::numeric_limits<std::uint32_t>::max();
    LayoutGdsLibrary library_;
    LayoutGdsParseMetrics metrics_;
    std::vector<LayoutDiagnostic> diagnostics_;
    std::optional<std::uint32_t> current_cell_;
    std::optional<LayoutGdsElement> current_element_;
    std::optional<LayoutGdsReference> current_reference_;
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen_layer_samples_;
    bool detailed_finalize_probes_ = detailedGdsFinalizeProbesEnabled();
    static constexpr std::size_t kUnsupportedRecordSampleLimit = 4U;
    struct UnsupportedRecordAggregate {
        std::size_t count = 0;
        std::vector<std::size_t> sample_offsets{};
    };
    std::map<GdsRecordType, UnsupportedRecordAggregate> unsupported_records_;
};

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

std::vector<std::uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Failed to read layout file: " + path.generic_string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("Failed to determine layout file size: " + path.generic_string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
            throw std::runtime_error("Failed to read complete layout file: " + path.generic_string());
        }
    }
    return bytes;
}

} // namespace

ParseResult<LayoutLefLibrary> parseLef(std::string_view text, std::string_view file_name) {
    return LefParser(text, file_name).parse();
}

ParseResult<LayoutDefDesign> parseDef(std::string_view text, std::string_view file_name) {
    return DefParser(text, file_name).parse();
}

ParseResult<LayoutGdsLibrary> parseGds(const std::vector<std::uint8_t>& bytes,
                                       std::string_view file_name) {
    return GdsParser(bytes, file_name).parse();
}

ParseResult<LayoutGdsLibrary> parseGds(const std::vector<std::uint8_t>& bytes,
                                       std::string_view file_name,
                                       LayoutGdsParseControl* control) {
    return GdsParser(bytes, file_name, control).parse();
}

ParseResult<LayoutLefLibrary> parseLefFile(const std::filesystem::path& path) {
    return parseLef(readTextFile(path), path.generic_string());
}

ParseResult<LayoutDefDesign> parseDefFile(const std::filesystem::path& path) {
    return parseDef(readTextFile(path), path.generic_string());
}

ParseResult<LayoutGdsLibrary> parseGdsFile(const std::filesystem::path& path) {
    return parseGdsFile(path, nullptr);
}

ParseResult<LayoutGdsLibrary> parseGdsFile(const std::filesystem::path& path,
                                           LayoutGdsParseControl* control) {
    const auto read_start = Clock::now();
    auto bytes = readBinaryFile(path);
    const auto read_micros = elapsedMicros(read_start);
    if (control != nullptr && control->publish_progress) {
        control->publish_progress(LayoutGdsParseProgress{
            .phase = LayoutSourcePhase::Read,
            .file_size_bytes = bytes.size(),
            .bytes_read = bytes.size(),
        });
    }
    auto result = parseGds(bytes, path.generic_string(), control);
    result.value.parse_metrics.read_micros = read_micros;
    return result;
}

} // namespace pristine::layout
