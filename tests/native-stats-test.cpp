#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include "../native/stats-reader.hpp"
using json = nlohmann::json;
using path = std::filesystem::path;
static int at = 0, passed = 0;
const std::string A = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", B = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
json row(std::string type, json payload, std::string timestamp = {}) {
    if (timestamp.empty()) { ++at; char buffer[64]; std::snprintf(buffer, sizeof(buffer), "2026-10-10T00:%02d:%02d.000Z", at / 60, at % 60); timestamp = buffer; }
    return {{"timestamp", timestamp}, {"type", type}, {"payload", payload}};
}
json event(std::string type, json payload = json::object()) { payload["type"] = type; return row("event_msg", payload); }
json U(int input, int output, int cached = 0) { return {{"input_tokens", input}, {"output_tokens", output}, {"cached_input_tokens", cached}, {"total_tokens", input + output}}; }
json count(json total, json last, int window = 1000) { return event("token_count", {{"info", {{"total_token_usage", total}, {"last_token_usage", last}, {"model_context_window", window}}}}); }
json token(std::string id, std::string turn, json total, json turnUsage, json request) { return row("token_usage_record", {{"thread_id", id}, {"turn_id", turn}, {"root_turn_id", turn}, {"thread_token_usage", total}, {"turn_token_usage", turnUsage}, {"usage", request}}); }
void append(const path& file, std::initializer_list<json> records) { std::ofstream out(file, std::ios::app | std::ios::binary); for (const auto& record : records) out << record.dump() << '\n'; }
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void check(const char* message, std::function<void()> run) { run(); std::cout << "PASS " << message << '\n'; ++passed; }
bool equal(double a, double b) { return std::abs(a - b) < 0.0000001; }

int main(int argc, char** argv) {
    if (argc == 3) {
        SessionStatsReader reader(std::filesystem::u8path(argv[1]));
        std::cout << reader.read(argv[2]).dump() << '\n'; return 0;
    }
    const path work = std::filesystem::absolute(path(__FILE__).parent_path());
    const path fixture = work / ("native-stats-fixtures-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directory(fixture);
    const path a = fixture / L"a.jsonl", b = fixture / L"b.jsonl", replacement = fixture / L"replacement.jsonl", index = fixture / L"session_index.jsonl";
    try {
        std::ofstream(a).close(); std::ofstream(b).close();
        { std::ofstream out(index, std::ios::binary); out << json({{"id", A}, {"thread_name", "selected fixture name"}}).dump() << '\n'; out << json({{"id", B}, {"thread_name", "other fixture name"}}).dump() << '\n'; }
        SessionStatsReader reader(fixture);
        check("empty session has zero tokens and null speed", [&] { auto s = reader.readFile(A, a); require(s["totalTokens"] == 0 && s["averageOutputTps"].is_null() && s["status"] == "idle", "empty"); });
        append(a, {event("task_started", {{"turn_id", "one"}}), row("turn_context", {{"turn_id", "one"}, {"model", "fixture-model-a"}}), count(U(100, 10, 50), U(100, 10, 50)), count(U(100, 10, 50), U(100, 10, 50)), count(U(300, 30, 150), U(200, 20, 100)), event("task_complete", {{"turn_id", "one"}, {"duration_ms", 6000}, {"time_to_first_token_ms", 400}})});
        check("legacy cumulative counts and mirrors deduplicate", [&] { auto s = reader.readFile(A, a); require(s["totalTokens"] == 330 && equal(s["averageOutputTps"], 5) && s["status"] == "completed", "legacy"); });
        check("input, cached input, last completed turn and TTFT are preserved", [&] { auto s = reader.readFile(A, a); require(s["inputTokens"] == 300 && s["cachedInputTokens"] == 150 && equal(s["cacheHitPercent"], 50) && s["lastTurnTokens"] == 330 && s["lastTtftMs"] == 400 && s["lastTurnDurationMs"] == 6000, "full fields"); });
        check("selected title, model and latest request context fallback are correct", [&] { auto s = reader.readFile(A, a); require(s["title"] == "selected fixture name" && s["model"] == "fixture-model-a" && s["contextUsed"] == 220 && s["contextWindow"] == 1000 && equal(s["contextPercent"], 22), "title model context"); });
        check("unchanged file returns same cached summary", [&] { auto expected = reader.readFile(A, a); for (int i = 0; i < 100; ++i) require(reader.readFile(A, a) == expected, "cache"); });
        const auto start = event("task_started", {{"turn_id", "two"}}).dump();
        { std::ofstream out(a, std::ios::app | std::ios::binary); out << start.substr(0, 21); }
        check("partial line is not counted before completion", [&] { auto s = reader.readFile(A, a); require(s["status"] == "completed" && s["totalTokens"] == 330, "partial"); });
        { std::ofstream out(a, std::ios::app | std::ios::binary); out << start.substr(21) << '\n'; }
        check("completed partial start changes running status", [&] { auto s = reader.readFile(A, a); require(s["status"] == "running" && equal(s["averageOutputTps"], 5), "partial finished"); });
        check("new running turn keeps the prior completed-turn fields", [&] { auto s = reader.readFile(A, a); require(s["lastTurnTokens"] == 330 && s["lastTtftMs"] == 400, "stable completion"); });
        append(a, {token(A, "two", U(700, 70), U(400, 40), U(400, 40)), count(U(700, 70), U(400, 40)), count(U(300, 30), U(200, 20)), event("task_complete", {{"turn_id", "two"}, {"duration_ms", 4000}})});
        check("modern cumulative and stale legacy mirrors share one total", [&] { auto s = reader.readFile(A, a); require(s["totalTokens"] == 770 && equal(s["averageOutputTps"], 7), "modern"); });
        check("switch to another thread frees old summary and isolates usage", [&] { require(reader.readFile(B, b)["totalTokens"] == 0, "isolated"); require(reader.readFile(A, a)["totalTokens"] == 770, "switch restore"); });
        append(a, {event("task_started", {{"turn_id", "three"}}), token(A, "three", U(800, 80), U(100, 10), U(100, 10)), row("compacted", {{"message", "do not retain this summary"}}), token(A, "three", U(50, 5), U(50, 5), U(50, 5)), event("task_complete", {{"turn_id", "three"}, {"duration_ms", 5000}})});
        check("compaction counter reset retains whole-turn usage and historical total", [&] { auto s = reader.readFile(A, a); require(s["totalTokens"] == 935 && equal(s["averageOutputTps"], 85.0 / 15), "compaction"); });
        check("compaction count, last turn and post-compaction context reflect complete turn", [&] { auto s = reader.readFile(A, a); require(s["compactionCount"] == 1 && s["lastTurnTokens"] == 165 && s["contextUsed"] == 55, "compaction fields"); });
        append(a, {event("task_started", {{"turn_id", "aborted"}}), token(A, "aborted", U(70, 15), U(20, 10), U(20, 10)), event("turn_aborted", {{"turn_id", "aborted"}})});
        check("aborted turn is consumed but excluded from completed-turn speed", [&] { auto s = reader.readFile(A, a); require(s["totalTokens"] == 965 && equal(s["averageOutputTps"], 85.0 / 15) && s["status"] == "paused", "aborted"); });
        append(a, {token(B, "foreign", U(900000, 10000), U(900000, 10000), U(900000, 10000)), row("token_usage_record", {{"thread_id", A}, {"turn_id", "child"}, {"root_turn_id", "aborted"}, {"thread_token_usage", U(800000, 10000)}})});
        check("foreign and child-agent records are excluded", [&] { require(reader.readFile(A, a)["totalTokens"] == 965, "child excluded"); });
        append(a, {event("task_started", {{"turn_id", "four"}}), token(A, "four", U(80, 17), U(10, 2), U(10, 2))});
        { std::ofstream out(a, std::ios::app | std::ios::binary); out << "{\"timestamp\":\"2026-10-10T00:01:00.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"function_call_output\",\"output\":\""; for (int i = 0; i < 20000000; ++i) out.put('x'); out << "\"}}\n"; }
        // The relevant completion's duration appears after a 20 MB ignored body.
        { std::ofstream out(a, std::ios::app | std::ios::binary); out << "{\"timestamp\":\"2026-10-10T00:02:00.000Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"four\",\"last_agent_message\":\""; for (int i = 0; i < 20000000; ++i) out.put('y'); out << "\",\"duration_ms\":2000}}\n"; }
        check("giant tool and assistant strings stream without losing later timing fields", [&] { auto s = reader.readFile(A, a); require(s["totalTokens"] == 977 && equal(s["averageOutputTps"], 87.0 / 17) && s["status"] == "completed", "giant line"); });
        auto duplicated = event("task_complete", {{"turn_id", "four"}, {"duration_ms", 2000}});
        append(a, {duplicated, duplicated});
        check("repeated completion does not add duration or output again", [&] { auto s = reader.readFile(A, a); require(equal(s["averageOutputTps"], 87.0 / 17), "duplicate completion"); });
        { std::ofstream out(a, std::ios::binary | std::ios::trunc); out << count(U(20, 2), U(20, 2)).dump() << '\n'; }
        check("file truncation rebuilds summary", [&] { auto s = reader.readFile(A, a); require(s["totalTokens"] == 22 && s["averageOutputTps"].is_null(), "truncation"); });
        { std::ofstream out(replacement, std::ios::binary); out << count(U(60, 6), U(60, 6)).dump() << '\n'; }
        std::filesystem::remove(a); std::filesystem::rename(replacement, a);
        check("same-path file identity replacement rebuilds summary", [&] { require(reader.readFile(A, a)["totalTokens"] == 66, "replacement"); });
        std::ofstream(b, std::ios::binary | std::ios::trunc).close();
        auto req = row("token_usage_record", {{"thread_id", B}, {"turn_id", "request-only"}, {"root_turn_id", "request-only"}, {"response_id", "request-1"}, {"usage", U(20, 10)}});
        append(b, {event("task_started", {{"turn_id", "request-only"}}), req, req, event("task_complete", {{"turn_id", "request-only"}, {"duration_ms", 2000}})});
        check("request-only modern response ID deduplicates", [&] { auto s = reader.readFile(B, b); require(s["totalTokens"] == 30 && equal(s["averageOutputTps"], 5), "request only"); });
        append(b, {event("thread_settings_applied", {{"thread_id", B}, {"thread_settings", {{"model", "fixture-model-a"}, {"model_context_window", 1000}}}}), row("turn_context", {{"model", "fixture-model-b"}})});
        check("switching models clears the old fallback window", [&] { auto s = reader.readFile(B, b); require(s["model"] == "fixture-model-b" && s["contextWindow"].is_null() && s["contextPercent"].is_null(), "model window clear"); });
        append(b, {count(U(40, 20, 10), U(20, 10, 10), 2000)});
        check("actual recorded model window is used for fallback percent", [&] { auto s = reader.readFile(B, b); require(s["contextUsed"] == 30 && s["contextWindow"] == 2000 && equal(s["contextPercent"], 1.5), "new model window"); });
        append(b, {row("compacted", {{"message", "do not return summary"}})});
        check("compaction clears stale fallback context without deleting consumption", [&] { auto s = reader.readFile(B, b); require(s["contextUsed"].is_null() && s["contextPercent"].is_null() && s["totalTokens"] == 60 && s["compactionCount"] == 1, "clear context"); });
        check("invalid thread ID returns null without reading a path", [&] { auto s = reader.readFile("../../auth.json", a); require(s["threadId"].is_null() && s["totalTokens"] == 0, "invalid ID"); });
        std::cout << passed << " native stats tests passed.\n";
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
    // Only explicitly created files are removed; no recursive directory delete.
    for (const auto& file : {a, b, replacement, index}) { std::error_code ec; std::filesystem::remove(file, ec); }
    std::filesystem::remove(fixture);
}
