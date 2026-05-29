#include "CompletionProvider.h"

#include "pristine/analysis/SourceUtil.h"

#include <cctype>

namespace pristine::analysis::semantic {
namespace {

bool isIdentifierStart(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) != 0 || value == '_' || value == '$';
}

bool isIdentifierContinue(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) != 0 || value == '_' || value == '$';
}

bool hasOnlyWhitespaceSinceLineStart(std::string_view text, size_t offset) {
    size_t line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n' && text[line_start - 1] != '\r') {
        --line_start;
    }
    for (size_t index = line_start; index < offset; ++index) {
        if (std::isspace(static_cast<unsigned char>(text[index])) == 0) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> qualifierBefore(std::string_view text, size_t qualifier_end) {
    size_t name_start = qualifier_end;
    while (name_start > 0 && isIdentifierContinue(text[name_start - 1])) {
        --name_start;
    }
    const auto qualifier = text.substr(name_start, qualifier_end - name_start);
    if (qualifier.empty() || !isIdentifierStart(qualifier.front())) {
        return std::nullopt;
    }
    return std::string(qualifier);
}

} // namespace

std::optional<size_t> completionPrefixStartOffset(std::string_view text,
                                                  int line,
                                                  int character,
                                                  std::string_view prefix) {
    const auto offset = utf8OffsetAtUtf16Position(text, line, character);
    if (!offset.has_value() || *offset < prefix.size()) {
        return std::nullopt;
    }
    return *offset - prefix.size();
}

CompletionContext detectCompletionContext(std::string_view text,
                                          int line,
                                          int character,
                                          std::string_view prefix) {
    CompletionContext context;
    context.prefix_start = completionPrefixStartOffset(text, line, character, prefix);
    if (!context.prefix_start.has_value()) {
        return context;
    }

    const auto prefix_start = *context.prefix_start;
    context.macro_invocation = prefix_start > 0 && text[prefix_start - 1] == '`';
    context.member_access = prefix_start > 0 && text[prefix_start - 1] == '.';
    context.module_instantiation_position = hasOnlyWhitespaceSinceLineStart(text, prefix_start);

    if (prefix_start >= 2 && text[prefix_start - 1] == ':' && text[prefix_start - 2] == ':') {
        context.package_qualifier = qualifierBefore(text, prefix_start - 2);
    }
    if (context.member_access) {
        context.member_qualifier = qualifierBefore(text, prefix_start - 1);
    }
    return context;
}

} // namespace pristine::analysis::semantic
