#include "pristine/analysis/SymbolIndex.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace pristine::analysis {
namespace {

std::string symbolKindLabel(int kind) {
    switch (kind) {
        case 2:
            return "Module";
        case 3:
            return "Namespace";
        case 4:
            return "Package";
        case 5:
            return "Class";
        case 10:
            return "Enum";
        case 11:
            return "Interface / Modport";
        case 12:
            return "Callable";
        case 13:
            return "Variable";
        case 14:
            return "Parameter";
        case 19:
            return "Instance";
        case 22:
            return "Enum Member";
        case 26:
            return "Typedef";
        default:
            return "Symbol";
    }
}

bool rangesEqual(const ParseRange& lhs, const ParseRange& rhs) {
    return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
}

void appendSymbolEntries(std::vector<SymbolEntry>& entries,
                         std::string_view uri,
                         const DocumentSymbol& symbol) {
    entries.push_back(SymbolEntry{.name = symbol.name,
                                  .kind = symbol.kind,
                                  .location = Location{.uri = std::string(uri),
                                                       .range = symbol.selection_range},
                                  .selection_range = symbol.selection_range});

    for (const auto& child : symbol.children) {
        appendSymbolEntries(entries, uri, child);
    }
}

bool fuzzyMatch(std::string_view query, std::string_view candidate) {
    if (query.empty()) {
        return true;
    }

    auto query_it = query.begin();
    for (auto candidate_it = candidate.begin();
         query_it != query.end() && candidate_it != candidate.end(); ++candidate_it) {
        const auto query_char = static_cast<char>(std::tolower(static_cast<unsigned char>(*query_it)));
        const auto candidate_char = static_cast<char>(std::tolower(static_cast<unsigned char>(*candidate_it)));
        if (query_char == candidate_char) {
            ++query_it;
        }
    }

    return query_it == query.end();
}

bool startsWithInsensitive(std::string_view prefix, std::string_view candidate) {
    if (prefix.size() > candidate.size()) {
        return false;
    }

    for (size_t index = 0; index < prefix.size(); ++index) {
        const auto prefix_char = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[index])));
        const auto candidate_char = static_cast<char>(std::tolower(static_cast<unsigned char>(candidate[index])));
        if (prefix_char != candidate_char) {
            return false;
        }
    }

    return true;
}

bool isDeclarationReference(const ReferenceEntry& reference, const std::vector<SymbolEntry>& symbols) {
    return std::any_of(symbols.begin(), symbols.end(), [&](const SymbolEntry& symbol) {
        return symbol.name == reference.name && symbol.location.uri == reference.location.uri &&
               rangesEqual(symbol.selection_range, reference.location.range);
    });
}

} // namespace

void SymbolIndex::clear() {
    documents_.clear();
}

void SymbolIndex::updateDocument(std::string_view uri, std::string_view text) {
    IndexedDocument indexed{};
    try {
        for (const auto& symbol : compilation_service_.documentSymbols(text, uri)) {
            appendSymbolEntries(indexed.symbols, uri, symbol);
        }
    }
    catch (...) {
        indexed.symbols.clear();
    }

    for (const auto& identifier : compilation_service_.identifiers(text)) {
        indexed.references.push_back(ReferenceEntry{.name = identifier.name,
                                                    .location = Location{.uri = std::string(uri),
                                                                         .range = identifier.range}});
    }

    documents_.insert_or_assign(std::string(uri), std::move(indexed));
}

void SymbolIndex::removeDocument(std::string_view uri) {
    documents_.erase(std::string(uri));
}

std::vector<SymbolEntry> SymbolIndex::definitions(std::string_view name,
                                                  std::string_view preferred_uri) const {
    std::vector<SymbolEntry> result;
    const auto append_matches = [&](const auto& document) {
        for (const auto& symbol : document.second.symbols) {
            if (symbol.name == name) {
                result.push_back(symbol);
            }
        }
    };

    const auto preferred_it = documents_.find(std::string(preferred_uri));
    if (preferred_it != documents_.end()) {
        append_matches(*preferred_it);
    }

    for (const auto& document : documents_) {
        if (document.first == preferred_uri) {
            continue;
        }
        append_matches(document);
    }

    return result;
}

std::vector<ReferenceEntry> SymbolIndex::references(std::string_view name,
                                                    bool include_declaration) const {
    std::vector<ReferenceEntry> result;
    for (const auto& document : documents_) {
        for (const auto& reference : document.second.references) {
            if (reference.name != name) {
                continue;
            }
            if (!include_declaration && isDeclarationReference(reference, document.second.symbols)) {
                continue;
            }
            result.push_back(reference);
        }
    }

    std::sort(result.begin(), result.end(), [](const ReferenceEntry& lhs, const ReferenceEntry& rhs) {
        if (lhs.location.uri != rhs.location.uri) {
            return lhs.location.uri < rhs.location.uri;
        }
        if (lhs.location.range.start_line != rhs.location.range.start_line) {
            return lhs.location.range.start_line < rhs.location.range.start_line;
        }
        return lhs.location.range.start_character < rhs.location.range.start_character;
    });

    return result;
}

std::vector<SymbolEntry> SymbolIndex::workspaceSymbols(std::string_view query) const {
    std::vector<SymbolEntry> result;
    for (const auto& document : documents_) {
        for (const auto& symbol : document.second.symbols) {
            if (fuzzyMatch(query, symbol.name)) {
                result.push_back(symbol);
            }
        }
    }

    std::sort(result.begin(), result.end(), [](const SymbolEntry& lhs, const SymbolEntry& rhs) {
        if (lhs.name != rhs.name) {
            return lhs.name < rhs.name;
        }
        return lhs.location.uri < rhs.location.uri;
    });

    return result;
}

std::vector<CompletionEntry> SymbolIndex::completions(std::string_view prefix,
                                                      std::string_view preferred_uri) const {
    std::vector<CompletionEntry> result;
    std::unordered_set<std::string> labels;

    const auto append_matches = [&](const auto& document) {
        for (const auto& symbol : document.second.symbols) {
            if (!startsWithInsensitive(prefix, symbol.name) || labels.contains(symbol.name)) {
                continue;
            }
            labels.insert(symbol.name);
            result.push_back(CompletionEntry{.label = symbol.name,
                                             .kind = symbol.kind,
                                             .detail = symbolKindLabel(symbol.kind)});
        }
    };

    const auto preferred_it = documents_.find(std::string(preferred_uri));
    if (preferred_it != documents_.end()) {
        append_matches(*preferred_it);
    }

    for (const auto& document : documents_) {
        if (document.first == preferred_uri) {
            continue;
        }
        append_matches(document);
    }

    return result;
}

} // namespace pristine::analysis
