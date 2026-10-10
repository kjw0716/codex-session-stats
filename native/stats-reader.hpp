#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include "vendor/json.hpp"

namespace session_monitor_detail {
using json = nlohmann::json;

inline std::string lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return text;
}
inline bool validId(std::string_view id) {
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') || (id[i] >= 'A' && id[i] <= 'F'))) return false;
    }
    return true;
}
inline std::string stringAt(const json& value, const char* key) {
    auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
inline std::optional<double> numberAt(const json& value, const char* key) {
    auto found = value.find(key);
    if (found == value.end() || !found->is_number()) return std::nullopt;
    double result = found->get<double>();
    return std::isfinite(result) && result >= 0 ? std::optional<double>(result) : std::nullopt;
}
inline std::optional<double> timestamp(std::string value) {
    // The rollout's UTC ISO timestamps are converted without locale/timezone APIs.
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value.back() != 'Z') return std::nullopt;
    auto digits = [&](std::size_t start, std::size_t count) -> int {
        int n = 0;
        for (std::size_t i = start; i < start + count; ++i) { if (i >= value.size() || value[i] < '0' || value[i] > '9') return -1; n = n * 10 + value[i] - '0'; }
        return n;
    };
    int y = digits(0, 4), m = digits(5, 2), d = digits(8, 2), h = digits(11, 2), min = digits(14, 2), sec = digits(17, 2);
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31 || h < 0 || h > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) return std::nullopt;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * unsigned(m + (m > 2 ? -3 : 9)) + 2) / 5 + unsigned(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    double ms = (double(era * 146097 + int(doe) - 719468) * 86400 + h * 3600 + min * 60 + sec) * 1000;
    if (value.size() > 20 && value[19] == '.') {
        double unit = 100;
        for (std::size_t i = 20; i < value.size() - 1 && unit >= 0.001; ++i, unit /= 10) {
            if (value[i] < '0' || value[i] > '9') return std::nullopt;
            ms += (value[i] - '0') * unit;
        }
    }
    return ms;
}
inline std::optional<double> timeAt(const json& value, const char* key) {
    if (auto n = numberAt(value, key)) return n;
    return timestamp(stringAt(value, key));
}
inline std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) {
    return b > std::numeric_limits<std::uint64_t>::max() - a ? std::numeric_limits<std::uint64_t>::max() : a + b;
}
struct Usage { std::uint64_t total = 0, output = 0, input = 0, cachedInput = 0; };
inline Usage plus(Usage a, Usage b) { return {saturatingAdd(a.total, b.total), saturatingAdd(a.output, b.output), saturatingAdd(a.input, b.input), saturatingAdd(a.cachedInput, b.cachedInput)}; }
inline Usage difference(Usage a, Usage b) { return {a.total > b.total ? a.total - b.total : 0, a.output > b.output ? a.output - b.output : 0, a.input > b.input ? a.input - b.input : 0, a.cachedInput > b.cachedInput ? a.cachedInput - b.cachedInput : 0}; }
inline std::optional<Usage> usage(const json& object) {
    if (!object.is_object()) return std::nullopt;
    auto total = numberAt(object, "total_tokens"), input = numberAt(object, "input_tokens"), output = numberAt(object, "output_tokens");
    if (!total) total = numberAt(object, "totalTokens");
    if (!input) input = numberAt(object, "inputTokens");
    if (!output) output = numberAt(object, "outputTokens");
    auto cached = numberAt(object, "cached_input_tokens");
    if (!cached) cached = numberAt(object, "cachedInputTokens");
    if (!total && !input && !output) return std::nullopt;
    auto integer = [](double v) { return v >= double(std::numeric_limits<std::uint64_t>::max()) ? std::numeric_limits<std::uint64_t>::max() : std::uint64_t(v); };
    return Usage{integer(total.value_or(input.value_or(0) + output.value_or(0))), integer(output.value_or(0)), integer(input.value_or(0)), integer((std::min)(input.value_or(0), cached.value_or(0)))};
}

// A JSONL projection lexer. Unselected string values (including multi-megabyte
// assistant/tool text) are consumed without ever being buffered. It retains only
// a small set of primitive token/event fields, not the full JSON/event history.
class ProjectionLexer {
    struct Frame { bool object; unsigned stage; std::string path, key; };
    enum class Mode { Normal, String, Primitive };
    Mode mode_ = Mode::Normal;
    std::vector<Frame> stack_;
    json projected_ = json::object();
    std::string token_, tokenPath_;
    bool key_ = false, capture_ = false, escaped_ = false, invalid_ = false, started_ = false, ended_ = false, skip_ = false;

    static bool selected(const std::string& p) {
        if (p == "id" || p == "thread_name" || p == "type" || p == "timestamp" || p == "payload.type" || p == "payload.id" || p == "payload.thread_id" || p == "payload.turn_id" || p == "payload.root_turn_id" || p == "payload.response_id" || p == "payload.duration_ms" || p == "payload.time_to_first_token_ms" || p == "payload.started_at" || p == "payload.completed_at" || p == "payload.model" || p == "payload.model_context_window" || p == "payload.info.model_context_window" || p == "payload.thread_settings.model" || p == "payload.thread_settings.model_context_window") return true;
        auto last = p.rfind('.');
        if (last == std::string::npos) return false;
        const std::string field = p.substr(last + 1), object = p.substr(0, last);
        return (field == "total_tokens" || field == "output_tokens" || field == "input_tokens" || field == "cached_input_tokens" || field == "totalTokens" || field == "outputTokens" || field == "inputTokens" || field == "cachedInputTokens") &&
            (object == "payload.usage" || object == "payload.turn_token_usage" || object == "payload.thread_token_usage" || object == "payload.info.total_token_usage" || object == "payload.info.last_token_usage");
    }
    std::string valuePath() const {
        if (stack_.empty()) return {};
        const auto& frame = stack_.back();
        if (!frame.object || frame.path == "!" || frame.path.size() + frame.key.size() > 200) return "!";
        return frame.path.empty() ? frame.key : frame.path + "." + frame.key;
    }
    void completeValue() {
        if (stack_.empty()) ended_ = true;
        else stack_.back().stage = 3;
    }
    bool expectingValue() const { return stack_.empty() ? !started_ : stack_.back().stage == 2; }
    void storeToken() {
        if (!capture_) return;
        json parsed = json::parse(token_, nullptr, false);
        if (parsed.is_discarded()) { invalid_ = true; return; }
        if (key_) {
            if (!parsed.is_string() || stack_.empty()) { invalid_ = true; return; }
            stack_.back().key = parsed.get<std::string>();
            stack_.back().stage = 1;
            return;
        }
        json* target = &projected_;
        std::size_t start = 0;
        while (true) {
            auto dot = tokenPath_.find('.', start);
            const auto part = tokenPath_.substr(start, dot == std::string::npos ? dot : dot - start);
            if (dot == std::string::npos) { (*target)[part] = std::move(parsed); break; }
            target = &(*target)[part]; start = dot + 1;
        }
        if (tokenPath_ == "type") {
            const std::string type = stringAt(projected_, "type");
            if (type != "event_msg" && type != "token_usage_record" && type != "compacted" && type != "compaction" && type != "turn_context" && type != "session_meta") skip_ = true;
        }
    }
    void normal(char c) {
        if (c == ' ' || c == '\t' || c == '\r') return;
        if (c == '{' || c == '[') {
            if (!expectingValue() || stack_.size() >= 128) { invalid_ = true; return; }
            const auto p = valuePath(); started_ = true;
            stack_.push_back({c == '{', c == '{' ? 0u : 2u, p, {}});
        } else if (c == '}' || c == ']') {
            if (stack_.empty() || stack_.back().object != (c == '}') || (stack_.back().stage != 3 && stack_.back().stage != (c == '}' ? 0u : 2u))) { invalid_ = true; return; }
            stack_.pop_back(); completeValue();
        } else if (c == ':') {
            if (stack_.empty() || !stack_.back().object || stack_.back().stage != 1) { invalid_ = true; return; }
            stack_.back().stage = 2;
        } else if (c == ',') {
            if (stack_.empty() || stack_.back().stage != 3) { invalid_ = true; return; }
            stack_.back().stage = stack_.back().object ? 0u : 2u;
        } else if (c == '"') {
            key_ = !stack_.empty() && stack_.back().object && stack_.back().stage == 0;
            if (!key_ && !expectingValue()) { invalid_ = true; return; }
            tokenPath_ = key_ ? std::string{} : valuePath();
            capture_ = key_ || selected(tokenPath_); token_ = capture_ ? "\"" : ""; escaped_ = false; mode_ = Mode::String;
        } else {
            if (!expectingValue()) { invalid_ = true; return; }
            key_ = false; tokenPath_ = valuePath(); capture_ = selected(tokenPath_); token_ = capture_ ? std::string(1, c) : ""; mode_ = Mode::Primitive;
        }
    }
public:
    void reset() {
        mode_ = Mode::Normal; stack_.clear(); projected_ = json::object(); token_.clear(); tokenPath_.clear();
        key_ = capture_ = escaped_ = invalid_ = started_ = ended_ = skip_ = false;
    }
    template<class Callback> void consume(char c, Callback&& callback) {
        if (c == '\n') {
            if (!skip_ && !invalid_) {
                if (mode_ == Mode::Primitive) { storeToken(); completeValue(); mode_ = Mode::Normal; }
                if (ended_ && stack_.empty() && mode_ == Mode::Normal) callback(projected_);
            }
            reset(); return;
        }
        if (skip_ || invalid_) return;
        if (mode_ == Mode::String) {
            if (capture_) {
                token_.push_back(c);
                if (token_.size() > (key_ ? 256u : 1024u)) {
                    if (key_) { capture_ = false; stack_.back().key = "!"; }
                    else capture_ = false;
                    token_.clear();
                }
            }
            if (escaped_) { escaped_ = false; return; }
            if (c == '\\') { escaped_ = true; return; }
            if (c == '"') {
                storeToken();
                if (key_) { if (!capture_) stack_.back().stage = 1; }
                else completeValue();
                mode_ = Mode::Normal;
            }
        } else if (mode_ == Mode::Primitive) {
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\r') {
                storeToken(); completeValue(); mode_ = Mode::Normal; normal(c);
            } else if (capture_) {
                token_.push_back(c);
                if (token_.size() > 128) { capture_ = false; token_.clear(); }
            }
        } else normal(c);
    }
};

class BoundedHashes {
    std::deque<std::uint64_t> order_;
    std::unordered_set<std::uint64_t> values_;
    std::size_t limit_;
public:
    explicit BoundedHashes(std::size_t limit) : limit_(limit) {}
    bool insert(std::string_view text) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
        if (!values_.insert(hash).second) return false;
        order_.push_back(hash);
        if (order_.size() > limit_) { values_.erase(order_.front()); order_.pop_front(); }
        return true;
    }
    void clear() { order_.clear(); values_.clear(); }
};

class SystemSqlite {
    struct Database;
    struct Statement;
    HMODULE library_ = nullptr;
    using Open = int (__cdecl*)(const char*, Database**, int, const char*);
    using Close = int (__cdecl*)(Database*);
    using Prepare = int (__cdecl*)(Database*, const char*, int, Statement**, const char**);
    using Bind = int (__cdecl*)(Statement*, int, const char*, int, void (__cdecl*)(void*));
    using Step = int (__cdecl*)(Statement*);
    using Column = const unsigned char* (__cdecl*)(Statement*, int);
    using Finalize = int (__cdecl*)(Statement*);
    using Timeout = int (__cdecl*)(Database*, int);
    Open open_ = nullptr; Close close_ = nullptr; Prepare prepare_ = nullptr; Bind bind_ = nullptr; Step step_ = nullptr; Column column_ = nullptr; Finalize finalize_ = nullptr; Timeout timeout_ = nullptr;
    template<class Function> Function load(const char* name) {
        const auto address = GetProcAddress(library_, name);
        Function function = nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }
public:
    struct Metadata { std::filesystem::path rollout; std::string title, model; };
    SystemSqlite() {
        std::array<wchar_t, MAX_PATH + 32> directory{};
        auto count = GetSystemDirectoryW(directory.data(), MAX_PATH);
        if (!count || count >= MAX_PATH) return;
        std::filesystem::path libraryPath(std::wstring(directory.data(), count)); libraryPath /= L"winsqlite3.dll";
        library_ = LoadLibraryExW(libraryPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) return;
        open_ = load<Open>("sqlite3_open_v2"); close_ = load<Close>("sqlite3_close");
        prepare_ = load<Prepare>("sqlite3_prepare_v2"); bind_ = load<Bind>("sqlite3_bind_text");
        step_ = load<Step>("sqlite3_step"); column_ = load<Column>("sqlite3_column_text");
        finalize_ = load<Finalize>("sqlite3_finalize"); timeout_ = load<Timeout>("sqlite3_busy_timeout");
    }
    SystemSqlite(const SystemSqlite&) = delete;
    SystemSqlite& operator=(const SystemSqlite&) = delete;
    ~SystemSqlite() { if (library_) FreeLibrary(library_); }
    std::optional<Metadata> metadata(const std::filesystem::path& codexHome, const std::string& id) {
        if (!open_ || !close_ || !prepare_ || !bind_ || !step_ || !column_ || !finalize_) return std::nullopt;
        const auto utf8 = (codexHome / L"state_5.sqlite").u8string();
        const std::string databasePath(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        Database* database = nullptr;
        if (open_(databasePath.c_str(), &database, 1 /* SQLITE_OPEN_READONLY */, nullptr) != 0) { if (database) close_(database); return std::nullopt; }
        if (timeout_) timeout_(database, 150);
        Statement* statement = nullptr;
        std::optional<Metadata> result;
        const char* modernSql = "SELECT rollout_path, CASE WHEN name IS NOT NULL AND length(name) BETWEEN 1 AND 240 AND instr(name,char(10))=0 THEN name WHEN length(title) BETWEEN 1 AND 240 AND instr(title,char(10))=0 THEN title ELSE NULL END, model FROM threads WHERE id = ? LIMIT 1";
        const char* legacySql = "SELECT rollout_path, CASE WHEN length(title) BETWEEN 1 AND 240 AND instr(title,char(10))=0 THEN title ELSE NULL END, model FROM threads WHERE id = ? LIMIT 1";
        int prepared = prepare_(database, modernSql, -1, &statement, nullptr);
        if (prepared != 0) {
            if (statement) finalize_(statement);
            statement = nullptr;
            prepared = prepare_(database, legacySql, -1, &statement, nullptr);
        }
        if (prepared == 0) {
            if (bind_(statement, 1, id.c_str(), -1, nullptr /* caller owns string through finalize */) == 0 && step_(statement) == 100 /* SQLITE_ROW */) {
                if (const unsigned char* text = column_(statement, 0)) {
                    std::string pathUtf8(reinterpret_cast<const char*>(text));
                    if (pathUtf8.rfind("\\\\?\\", 0) == 0) pathUtf8.erase(0, 4);
                    Metadata meta; meta.rollout = std::filesystem::u8path(pathUtf8);
                    if (const unsigned char* title = column_(statement, 1)) meta.title = reinterpret_cast<const char*>(title);
                    if (const unsigned char* model = column_(statement, 2)) meta.model = reinterpret_cast<const char*>(model);
                    result = std::move(meta);
                }
            }
        }
        if (statement) finalize_(statement);
        close_(database);
        return result;
    }
};
} // namespace session_monitor_detail

// Single selected-thread summary. The authoritative SQLite rollout is streamed
// in physical order and never merged with sibling/ancestor/background-review
// files. This intentionally avoids the previous reader's historical multi-file
// merge; pre-canonical history absent from that file cannot be recovered here.
class SessionStatsReader {
    using json = nlohmann::json;
    using Usage = session_monitor_detail::Usage;
    struct Turn { std::string id; Usage baseline{}, usage{}; bool hasStart = false, usageKnown = false, active = false; std::optional<double> startedAt; };
    std::filesystem::path home_, selectedFile_;
    session_monitor_detail::SystemSqlite sqlite_;
    session_monitor_detail::ProjectionLexer lexer_;
    session_monitor_detail::BoundedHashes events_{512}, responses_{1024};
    std::string thread_, status_ = "idle", lastCompletedId_, limitation_, title_, model_, metadataModel_, indexTitle_;
    Usage total_{}, snapshot_{}, epochOffset_{};
    Turn turn_{};
    bool modernSnapshot_ = false, compactionSinceUsage_ = false, identityKnown_ = false, indexLoaded_ = false;
    std::uint64_t offset_ = 0, fileSize_ = 0, writeTime_ = 0, fileId_ = 0;
    DWORD volume_ = 0;
    double completedOutput_ = 0, completedDuration_ = 0;
    std::uint64_t compactionCount_ = 0;
    std::optional<std::uint64_t> lastTurnTokens_, contextUsed_, contextWindow_;
    std::optional<double> lastCompletedAt_, lastTtftMs_, lastTurnDurationMs_;
    std::chrono::steady_clock::time_point lookupAfter_{};

    static std::uint64_t wide(DWORD high, DWORD low) { return (std::uint64_t(high) << 32) | low; }
    void clearSummary() {
        lexer_.reset(); events_.clear(); responses_.clear(); status_ = "idle"; lastCompletedId_.clear();
        total_ = {}; snapshot_ = {}; epochOffset_ = {}; turn_ = {}; modernSnapshot_ = compactionSinceUsage_ = identityKnown_ = false;
        offset_ = fileSize_ = writeTime_ = fileId_ = 0; volume_ = 0; completedOutput_ = completedDuration_ = 0; lastCompletedAt_.reset();
        compactionCount_ = 0; lastTurnTokens_.reset(); lastTtftMs_.reset(); lastTurnDurationMs_.reset(); contextUsed_.reset(); contextWindow_.reset(); model_ = metadataModel_;
    }
    void select(std::string id) {
        if (thread_ == id) return;
        thread_ = std::move(id); selectedFile_.clear(); limitation_.clear(); title_.clear(); model_.clear(); metadataModel_.clear(); indexTitle_.clear(); indexLoaded_ = false; lookupAfter_ = {}; clearSummary();
    }
    json result() const {
        json out = {{"threadId", thread_.empty() ? json(nullptr) : json(thread_)}, {"totalTokens", total_.total}, {"averageOutputTps", completedDuration_ > 0 ? json(completedOutput_ / (completedDuration_ / 1000)) : json(nullptr)}, {"status", status_}};
        out["title"] = title_.empty() ? json(nullptr) : json(title_);
        out["model"] = model_.empty() ? json(nullptr) : json(model_);
        out["inputTokens"] = total_.input;
        out["outputTokens"] = total_.output;
        out["cachedInputTokens"] = total_.cachedInput;
        out["cacheHitPercent"] = total_.input > 0 ? json(double(total_.cachedInput) / double(total_.input) * 100) : json(nullptr);
        out["lastTurnTokens"] = lastTurnTokens_ ? json(*lastTurnTokens_) : json(nullptr);
        out["compactionCount"] = compactionCount_;
        out["lastTtftMs"] = lastTtftMs_ ? json(*lastTtftMs_) : json(nullptr);
        out["lastTurnDurationMs"] = lastTurnDurationMs_ ? json(*lastTurnDurationMs_) : json(nullptr);
        out["contextUsed"] = contextUsed_ ? json(*contextUsed_) : json(nullptr);
        out["contextWindow"] = contextWindow_ ? json(*contextWindow_) : json(nullptr);
        out["contextPercent"] = contextUsed_ && contextWindow_ && *contextWindow_ > 0 ? json(double(*contextUsed_) / double(*contextWindow_) * 100) : json(nullptr);
        if (!limitation_.empty()) out["limitation"] = limitation_;
        return out;
    }
    void process(const json& row) {
        using namespace session_monitor_detail;
        std::string type = stringAt(row, "type");
        if (type == "compacted" || type == "compaction") {
            if (events_.insert(row.dump())) { compactionSinceUsage_ = true; compactionCount_ = saturatingAdd(compactionCount_, 1); contextUsed_.reset(); }
            return;
        }
        auto payload = row.find("payload");
        if (payload == row.end() || !payload->is_object()) return;
        const json& p = *payload;
        if (type == "event_msg") type = stringAt(p, "type");
        if (type != "task_started" && type != "task_complete" && type != "turn_aborted" && type != "task_aborted" && type != "token_usage_record" && type != "token_count" && type != "compacted" && type != "compaction" && type != "turn_context" && type != "session_meta" && type != "thread_settings_applied") return;
        const auto thread = stringAt(p, "thread_id"), turnId = stringAt(p, "turn_id"), root = stringAt(p, "root_turn_id");
        if (!thread.empty() && lower(thread) != thread_) return;
        if (type == "session_meta" && lower(stringAt(p, "id")) != thread_) return;
        if (!turnId.empty() && !root.empty() && turnId != root) return;
        const auto at = timeAt(row, "timestamp");
        if (!events_.insert(row.dump())) return;
        const json empty = json::object();
        const auto settingsIt = p.find("thread_settings"), infoIt = p.find("info");
        const json& settings = settingsIt != p.end() && settingsIt->is_object() ? *settingsIt : empty;
        const json& info = infoIt != p.end() && infoIt->is_object() ? *infoIt : empty;
        auto model = stringAt(p, "model"); if (model.empty()) model = stringAt(settings, "model");
        if (!model.empty() && model != model_) { model_ = model; contextWindow_.reset(); contextUsed_.reset(); }
        auto window = numberAt(p, "model_context_window"); if (!window) window = numberAt(info, "model_context_window"); if (!window) window = numberAt(settings, "model_context_window");
        if (window && *window > 0 && *window < double(std::numeric_limits<std::uint64_t>::max())) contextWindow_ = std::uint64_t(*window);
        if (type == "task_started") {
            if ((!turnId.empty() && (turnId == turn_.id || turnId == lastCompletedId_)) || (at && lastCompletedAt_ && *at <= *lastCompletedAt_)) return;
            turn_ = {turnId.empty() ? "legacy-" + stringAt(row, "timestamp") : turnId, total_, {}, true, false, true, timeAt(p, "started_at")};
            if (!turn_.startedAt) turn_.startedAt = at;
            status_ = "running"; return;
        }
        if (type == "compaction" || type == "compacted") { compactionSinceUsage_ = true; compactionCount_ = saturatingAdd(compactionCount_, 1); contextUsed_.reset(); return; }
        if (type == "token_usage_record" || type == "token_count") {
            const bool modern = type == "token_usage_record";
            auto getUsage = [&](const json& object, const char* key) -> std::optional<Usage> { auto found = object.find(key); return found == object.end() ? std::nullopt : usage(*found); };
            auto cumulative = getUsage(p, "thread_token_usage"); if (!cumulative) cumulative = getUsage(info, "total_token_usage");
            auto request = getUsage(p, "usage"); if (!request) request = getUsage(info, "last_token_usage");
            auto turnUsage = getUsage(p, "turn_token_usage");
            bool accepted = true;
            if (cumulative) {
                if (!compactionSinceUsage_ && modernSnapshot_ && !modern && cumulative->total < snapshot_.total) accepted = false;
                if (accepted && cumulative->total < snapshot_.total) {
                    if (compactionSinceUsage_) epochOffset_ = total_;
                    else accepted = false;
                }
                if (accepted) { snapshot_ = *cumulative; modernSnapshot_ = modernSnapshot_ || modern; total_ = plus(epochOffset_, snapshot_); }
            } else if (request && modern) {
                const auto responseId = stringAt(p, "response_id");
                if (responses_.insert(responseId.empty() ? row.dump() : responseId)) { total_ = plus(total_, *request); snapshot_ = difference(total_, epochOffset_); modernSnapshot_ = true; }
                else accepted = false;
            } else if (request && !modernSnapshot_) {
                if (responses_.insert(stringAt(row, "timestamp") + "|" + std::to_string(request->total) + "|" + std::to_string(request->output))) total_ = plus(total_, *request);
                else accepted = false;
            }
            if (accepted) {
                if (request) contextUsed_ = saturatingAdd(request->input, request->output);
                if (!turn_.active && !turnId.empty() && turnId != lastCompletedId_) {
                    turn_ = {turnId, total_, {}, false, false, true, at};
                }
                if (turn_.active && (turnId.empty() || turnId == turn_.id)) {
                    turn_.usage = !turn_.hasStart && turnUsage ? *turnUsage : difference(total_, turn_.baseline);
                    turn_.usageKnown = true;
                }
                compactionSinceUsage_ = false;
            }
            return;
        }
        if (type == "task_complete") {
            if (!turnId.empty() && turnId == lastCompletedId_) return;
            if (at && lastCompletedAt_ && *at <= *lastCompletedAt_) return;
            if (!turn_.active || (!turnId.empty() && turnId != turn_.id)) return;
            auto duration = numberAt(p, "duration_ms");
            if (!duration) { const auto complete = timeAt(p, "completed_at"); if (complete && turn_.startedAt) duration = (std::max)(0.0, *complete - *turn_.startedAt); }
            if (turn_.usageKnown && duration && *duration > 0) { completedOutput_ += double(turn_.usage.output); completedDuration_ += *duration; }
            lastTurnTokens_ = turn_.usageKnown ? std::optional<std::uint64_t>(turn_.usage.total) : std::nullopt;
            lastTtftMs_ = numberAt(p, "time_to_first_token_ms"); lastTurnDurationMs_ = duration;
            lastCompletedId_ = turn_.id; lastCompletedAt_ = at; turn_.active = false; status_ = "completed"; return;
        }
        if ((type == "turn_aborted" || type == "task_aborted") && (turnId.empty() || turnId == turn_.id)) {
            turn_.active = false; status_ = "paused";
        }
    }
    bool streamSelected() {
        HANDLE file = CreateFileW(selectedFile_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info)) { CloseHandle(file); return false; }
        const auto size = wide(info.nFileSizeHigh, info.nFileSizeLow), modified = wide(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime), identity = wide(info.nFileIndexHigh, info.nFileIndexLow);
        if (identityKnown_ && (volume_ != info.dwVolumeSerialNumber || fileId_ != identity || size < offset_ || (size == fileSize_ && modified != writeTime_))) clearSummary();
        if (identityKnown_ && size == fileSize_ && modified == writeTime_) { CloseHandle(file); return true; }
        volume_ = info.dwVolumeSerialNumber; fileId_ = identity; identityKnown_ = true;
        LARGE_INTEGER position{}; position.QuadPart = LONGLONG(offset_);
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) { CloseHandle(file); return false; }
        std::array<char, 65536> block{};
        bool success = true;
        while (offset_ < size) {
            const DWORD wanted = DWORD((std::min)(std::uint64_t(block.size()), size - offset_)); DWORD read = 0;
            if (!ReadFile(file, block.data(), wanted, &read, nullptr)) { success = false; break; }
            if (!read) break;
            offset_ += read;
            for (DWORD i = 0; i < read; ++i) lexer_.consume(block[i], [this](const json& row) { process(row); });
        }
        fileSize_ = size; writeTime_ = modified; CloseHandle(file); return success;
    }
    void readIndexTitle() {
        if (indexLoaded_) return;
        indexLoaded_ = true;
        std::ifstream index(home_ / L"session_index.jsonl", std::ios::binary);
        if (!index) return;
        session_monitor_detail::ProjectionLexer projector;
        std::array<char, 16384> block{};
        auto retainSelected = [this](const json& row) {
            if (session_monitor_detail::lower(session_monitor_detail::stringAt(row, "id")) != thread_) return;
            const auto name = session_monitor_detail::stringAt(row, "thread_name");
            if (!name.empty() && name.size() <= 1024 && name.find('\n') == std::string::npos) indexTitle_ = name;
        };
        while (index) {
            index.read(block.data(), block.size());
            for (std::streamsize i = 0; i < index.gcount(); ++i) projector.consume(block[std::size_t(i)], retainSelected);
        }
        projector.consume('\n', retainSelected);
        if (!indexTitle_.empty()) title_ = indexTitle_;
    }
    void locate() {
        const auto current = std::chrono::steady_clock::now();
        if (current < lookupAfter_) return;
        lookupAfter_ = current + std::chrono::seconds(30);
        std::error_code ec;
        if (auto metadata = sqlite_.metadata(home_, thread_)) {
            if (indexTitle_.empty()) title_ = metadata->title;
            metadataModel_ = metadata->model;
            if (model_.empty()) model_ = metadataModel_;
            if (std::filesystem::is_regular_file(metadata->rollout, ec)) { selectedFile_ = metadata->rollout; limitation_.clear(); return; }
        }
        std::filesystem::path newest; std::filesystem::file_time_type newestTime{}; unsigned matches = 0;
        for (const auto& root : {home_ / L"sessions", home_ / L"archived_sessions"}) {
            ec.clear(); std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
            while (!ec && it != end) {
                const auto entry = *it; ec.clear();
                if (entry.is_regular_file(ec) && entry.path().extension() == L".jsonl") {
                    const auto utf8 = entry.path().filename().u8string(); const std::string filename(reinterpret_cast<const char*>(utf8.data()), utf8.size());
                    if (session_monitor_detail::lower(filename).find(thread_) != std::string::npos) {
                        ec.clear(); auto modified = entry.last_write_time(ec);
                        if (!ec) { ++matches; if (newest.empty() || modified > newestTime) { newest = entry.path(); newestTime = modified; } }
                    }
                }
                ec.clear(); it.increment(ec);
            }
        }
        selectedFile_ = newest;
        limitation_ = matches > 1 ? "canonical-unavailable; newest-matching-rollout-only" : "canonical-unavailable; matching-rollout-only";
    }
public:
    explicit SessionStatsReader(std::filesystem::path codexHome) : home_(std::move(codexHome)) {}
    json read(const std::string& threadId) {
        if (!session_monitor_detail::validId(threadId)) { select({}); return result(); }
        select(session_monitor_detail::lower(threadId));
        readIndexTitle();
        if (selectedFile_.empty()) locate();
        if (!selectedFile_.empty() && streamSelected()) return result();
        selectedFile_.clear(); locate();
        if (!selectedFile_.empty() && streamSelected()) return result();
        status_ = "unavailable"; limitation_ = "selected-rollout-unavailable"; return result();
    }
    // Fixture API uses exactly the production streaming/reducer path, without SQL
    // or directory discovery. Changing the path drops the previous summary.
    json readFile(const std::string& threadId, const std::filesystem::path& filename) {
        if (!session_monitor_detail::validId(threadId)) { select({}); return result(); }
        select(session_monitor_detail::lower(threadId));
        readIndexTitle();
        if (selectedFile_ != filename) { clearSummary(); selectedFile_ = filename; }
        limitation_.clear();
        if (!streamSelected()) { status_ = "unavailable"; limitation_ = "selected-rollout-unavailable"; }
        return result();
    }
};
