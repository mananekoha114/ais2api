// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/anthropic.hpp"
#include "ais2api/tokenizer.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace ais2api {
namespace {
// Thinking signatures this service issues wrap a Gemini thought signature.
// Signatures without it (from Anthropic or another gateway) never go upstream.
constexpr std::string_view signature_prefix = "ais2api.gemini.";

[[noreturn]] void invalid(const std::string &message) {
    throw Error(400, "invalid_argument", "Anthropic request: " + message);
}
const json::object &object_at(const json::value &value, std::string_view what) {
    if (!value.is_object()) invalid(std::string(what) + " must be an object");
    return value.as_object();
}
const json::array &array_at(const json::value &value, std::string_view what) {
    if (!value.is_array()) invalid(std::string(what) + " must be an array");
    return value.as_array();
}
std::string string_at(const json::value &value, std::string_view what) {
    if (!value.is_string()) invalid(std::string(what) + " must be a string");
    return std::string(value.as_string());
}
const json::value &required(const json::object &o, std::string_view key, std::string_view what) {
    auto p = o.if_contains(key);
    if (!p) invalid(std::string(what) + "." + std::string(key) + " is required");
    return *p;
}
bool flag(const json::object &o, std::string_view key, std::string_view what) {
    auto p = o.if_contains(key);
    if (!p || p->is_null()) return false;
    if (!p->is_bool()) invalid(std::string(what) + "." + std::string(key) + " must be a boolean");
    return p->as_bool();
}
void allow(const json::object &o, std::initializer_list<std::string_view> names, std::string_view what) {
    for (const auto &kv : o)
        if (std::find(names.begin(), names.end(), std::string_view(kv.key())) == names.end())
            invalid(std::string(what) + ": unsupported field " + std::string(kv.key()));
}
std::optional<std::string> own_signature(std::string_view value) {
    if (value.size() <= signature_prefix.size() || !value.starts_with(signature_prefix)) return std::nullopt;
    return std::string(value.substr(signature_prefix.size()));
}

// Records fields the upstream cannot honor: an error by default, a logged
// drop under ignore_unsupported_parameters.
struct Policy {
    bool discard;
    json::array &ignored;
    void note(std::string_view name) {
        json::value value(name);
        if (std::find(ignored.begin(), ignored.end(), value) == ignored.end()) ignored.push_back(std::move(value));
    }
    void unsupported(const std::string &name, std::string_view detail = {}) {
        if (!discard)
            invalid(name + " is not supported by this upstream" + (detail.empty() ? "" : "; " + std::string(detail)));
        note(name);
    }
};

std::string effort_level(const json::value &value, std::string_view what, bool allow_xhigh) {
    const auto effort = string_at(value, what);
    if (effort == "low") return "LOW";
    if (effort == "medium") return "MEDIUM";
    if (effort == "high") return "HIGH";
    // Only Hydrogen and Neon accept XHIGH; other models cap at HIGH.
    if (effort == "xhigh" || effort == "max") return allow_xhigh ? "XHIGH" : "HIGH";
    invalid(std::string(what) + " must be low, medium, high, xhigh or max");
}

std::string text_block(const json::object &block, std::string_view what) {
    // Citations on earlier text are metadata of a past answer.
    allow(block, {"type", "text", "cache_control", "citations"}, what);
    return string_at(required(block, "text", what), std::string(what) + ".text");
}
json::object base64_part(const json::object &source, std::string_view what, std::string_view kind) {
    allow(source, {"type", "media_type", "data"}, what);
    auto media = string_at(required(source, "media_type", what), std::string(what) + ".media_type");
    auto data = string_at(required(source, "data", what), std::string(what) + ".data");
    if (data.empty()) invalid(std::string(what) + ".data cannot be empty");
    if (kind == "image" ? !media.starts_with("image/") : media != "application/pdf")
        invalid(std::string(what) + ".media_type " + media + " is not supported for " + std::string(kind));
    return {{"inlineData", json::object{{"mimeType", media}, {"data", data}}}};
}
json::object image_part(const json::object &block) {
    allow(block, {"type", "source", "cache_control"}, "image block");
    auto &source = object_at(required(block, "source", "image block"), "image block.source");
    const auto type = string_or(source, "type");
    if (type != "base64")
        invalid("image source type " + type + " is not supported; send base64 data");
    return base64_part(source, "image block.source", "image");
}
void document_parts(const json::object &block, json::array &parts, Policy &policy) {
    allow(block, {"type", "source", "title", "context", "citations", "cache_control"}, "document block");
    if (auto c = block.if_contains("citations"); c && c->is_object() && flag(c->as_object(), "enabled", "document.citations"))
        policy.unsupported("document.citations");
    std::string header;
    for (auto key : {"title", "context"})
        if (auto p = block.if_contains(key); p && !p->is_null()) {
            auto value = string_at(*p, std::string("document block.") + key);
            if (!value.empty()) header += (header.empty() ? "" : "\n") + value;
        }
    if (!header.empty()) parts.push_back(json::object{{"text", header}});
    auto &source = object_at(required(block, "source", "document block"), "document block.source");
    const auto type = string_or(source, "type");
    if (type == "base64")
        parts.push_back(base64_part(source, "document block.source", "documents"));
    else if (type == "text") {
        allow(source, {"type", "media_type", "data"}, "document block.source");
        parts.push_back(json::object{{"text", string_at(required(source, "data", "document block.source"), "document block.source.data")}});
    } else if (type == "content") {
        allow(source, {"type", "content"}, "document block.source");
        auto &content = required(source, "content", "document block.source");
        if (content.is_string())
            parts.push_back(json::object{{"text", content}});
        else
            for (auto &item : array_at(content, "document block.source.content")) {
                auto &inner = object_at(item, "document content block");
                const auto inner_type = string_or(inner, "type");
                if (inner_type == "text") parts.push_back(json::object{{"text", text_block(inner, "document text block")}});
                else if (inner_type == "image") parts.push_back(image_part(inner));
                else invalid("document content block type " + inner_type + " is not supported");
            }
    } else
        invalid("document source type " + type + " is not supported; send base64, text or content");
}

// The declared tool set, changed in order by tool_addition and tool_removal.
struct Tools {
    std::vector<std::string> order;
    std::map<std::string, json::object> declarations;
    std::set<std::string> removed;

    json::object declare(const json::object &tool, Policy &policy, std::string_view what) {
        allow(tool, {"type", "name", "description", "input_schema", "cache_control", "defer_loading", "strict",
                     "input_examples", "allowed_callers", "eager_input_streaming"},
              what);
        auto name = string_at(required(tool, "name", what), std::string(what) + ".name");
        if (name.empty()) invalid(std::string(what) + ".name cannot be empty");
        if (flag(tool, "strict", what)) policy.unsupported("tools.strict");
        for (auto key : {"input_examples", "allowed_callers"})
            if (auto p = tool.if_contains(key); p && !p->is_null())
                policy.unsupported(std::string("tools.") + key);
        // defer_loading only delays when Anthropic shows a tool; Gemini sees
        // every declaration. eager_input_streaming changes nothing here.
        flag(tool, "defer_loading", what);
        flag(tool, "eager_input_streaming", what);
        json::object declaration{{"name", name}};
        if (auto p = tool.if_contains("description"); p && !p->is_null())
            declaration["description"] = string_at(*p, std::string(what) + ".description");
        declaration["parameters"] = object_at(required(tool, "input_schema", what), std::string(what) + ".input_schema");
        return declaration;
    }
    void add(json::object declaration) {
        const auto name = std::string(declaration.at("name").as_string());
        if (!declarations.contains(name)) order.push_back(name);
        declarations[name] = std::move(declaration);
        removed.erase(name);
    }
    json::array active() const {
        json::array out;
        for (const auto &name : order)
            if (!removed.contains(name)) out.push_back(declarations.at(name));
        return out;
    }
};
std::string tool_reference(const json::value &value, std::string_view what) {
    auto &tool = object_at(value, what);
    allow(tool, {"type", "name"}, what);
    if (string_or(tool, "type") != "tool_reference") invalid(std::string(what) + ".type must be tool_reference");
    return string_at(required(tool, "name", what), std::string(what) + ".name");
}

class Lowering {
  public:
    Lowering(Policy &policy, bool allow_xhigh) : policy_(policy), allow_xhigh_(allow_xhigh) {}

    void system(const json::value &value) {
        if (value.is_string()) {
            if (!value.as_string().empty()) system_.push_back(json::object{{"text", value}});
            return;
        }
        for (auto &item : array_at(value, "system")) {
            auto &block = object_at(item, "system block");
            if (string_or(block, "type") != "text") invalid("system blocks must be text");
            auto text = text_block(block, "system block");
            if (!text.empty()) system_.push_back(json::object{{"text", text}});
        }
    }

    void messages(const json::array &messages, const std::optional<std::size_t> &keep_thinking) {
        // With clear_thinking, only the last N assistant turns that think keep it.
        std::set<std::size_t> thinking_turns;
        for (std::size_t i = messages.size(); i-- > 0;) {
            auto &message = object_at(messages[i], "message");
            auto content = message.if_contains("content");
            if (string_or(message, "role") != "assistant" || !content || !content->is_array()) continue;
            for (auto &block : content->as_array())
                if (block.is_object()) {
                    const auto type = string_or(block.as_object(), "type");
                    if (type == "thinking" || type == "redacted_thinking") {
                        if (!keep_thinking || thinking_turns.size() < *keep_thinking) thinking_turns.insert(i);
                        break;
                    }
                }
        }
        std::optional<std::size_t> last_user;
        for (std::size_t i = 0; i < messages.size(); ++i)
            if (string_or(object_at(messages[i], "message"), "role") == "user") last_user = i;
        if (messages.empty()) invalid("messages cannot be empty");
        for (std::size_t i = 0; i < messages.size(); ++i) {
            auto &message = object_at(messages[i], "message");
            const auto role = string_or(message, "role");
            if (role == "system") {
                system_message(message, last_user && *last_user > i);
                continue;
            }
            allow(message, {"role", "content"}, "message");
            auto &content = required(message, "content", "message");
            if (role == "user") {
                auto parts = user_parts(content);
                // System text joins the user turn after its tool results, so a
                // function call is still followed directly by its responses.
                if (!pending_.empty()) {
                    auto at = std::find_if(parts.begin(), parts.end(), [](const json::value &part) {
                        return !part.as_object().if_contains("functionResponse");
                    });
                    const auto offset = static_cast<std::size_t>(at - parts.begin());
                    for (std::size_t k = 0; k < pending_.size(); ++k)
                        parts.insert(parts.begin() + static_cast<std::ptrdiff_t>(offset + k), pending_[k]);
                    pending_.clear();
                }
                append("user", std::move(parts));
            } else if (role == "assistant") {
                if (!pending_.empty()) append("user", std::exchange(pending_, {}));
                append("model", assistant_parts(content, thinking_turns.contains(i)));
            } else
                invalid("message role must be user, assistant or system");
        }
        if (!pending_.empty()) append("user", std::exchange(pending_, {}));
        if (contents_.empty()) invalid("messages contain no content");
    }

    Tools tools;
    std::optional<std::string> effort;
    json::array contents_, system_;

  private:
    void append(std::string_view role, json::array parts) {
        if (parts.empty()) return;
        if (!contents_.empty() && string_or(contents_.back().as_object(), "role") == role) {
            auto &existing = contents_.back().as_object().at("parts").as_array();
            for (auto &part : parts) existing.push_back(std::move(part));
            return;
        }
        contents_.push_back(json::object{{"role", role}, {"parts", std::move(parts)}});
    }

    void system_message(const json::object &message, bool later_user) {
        allow(message, {"role", "content", "clear_at", "output_config"}, "system message");
        bool cleared = false;
        if (auto p = message.if_contains("clear_at"); p && !p->is_null()) {
            if (string_at(*p, "system message.clear_at") != "next_user_message")
                invalid("system message.clear_at must be next_user_message");
            // Shown for one turn only.
            cleared = later_user;
        }
        if (auto p = message.if_contains("output_config"); p && !p->is_null()) {
            auto &config = object_at(*p, "system message.output_config");
            allow(config, {"effort"}, "system message.output_config");
            if (auto e = config.if_contains("effort"); e && !e->is_null())
                effort = effort_level(*e, "system message.output_config.effort", allow_xhigh_);
        }
        json::array parts;
        auto &content = required(message, "content", "system message");
        if (content.is_string()) {
            if (!content.as_string().empty()) parts.push_back(json::object{{"text", content}});
        } else
            for (auto &item : array_at(content, "system message.content")) {
                auto &block = object_at(item, "system message block");
                const auto type = string_or(block, "type");
                if (type == "text") {
                    auto text = text_block(block, "system message block");
                    if (!text.empty()) parts.push_back(json::object{{"text", text}});
                } else if (type == "tool_addition") {
                    allow(block, {"type", "tool", "cache_control"}, "tool_addition");
                    auto &tool = object_at(required(block, "tool", "tool_addition"), "tool_addition.tool");
                    if (string_or(tool, "type") == "tool_definition") {
                        allow(tool, {"type", "definition"}, "tool_addition.tool");
                        tools.add(tools.declare(object_at(required(tool, "definition", "tool_addition.tool"), "tool definition"),
                                                policy_, "tool definition"));
                    } else {
                        auto name = tool_reference(tool, "tool_addition.tool");
                        if (!tools.declarations.contains(name)) invalid("tool_addition names an undeclared tool");
                        tools.removed.erase(name);
                    }
                } else if (type == "tool_removal") {
                    allow(block, {"type", "tool", "cache_control"}, "tool_removal");
                    auto name = tool_reference(required(block, "tool", "tool_removal"), "tool_removal.tool");
                    if (!tools.declarations.contains(name)) invalid("tool_removal names an undeclared tool");
                    tools.removed.insert(name);
                } else
                    invalid("system message block type " + type + " is not supported");
            }
        if (cleared || parts.empty()) return;
        if (contents_.empty())
            for (auto &part : parts) system_.push_back(std::move(part));
        else if (string_or(contents_.back().as_object(), "role") == "user" && pending_.empty())
            append("user", std::move(parts));
        else
            for (auto &part : parts) pending_.push_back(std::move(part));
    }

    json::array user_parts(const json::value &content) {
        json::array parts;
        if (content.is_string()) {
            if (!content.as_string().empty()) parts.push_back(json::object{{"text", content}});
            return parts;
        }
        for (auto &item : array_at(content, "user message content")) {
            auto &block = object_at(item, "content block");
            const auto type = string_or(block, "type");
            if (type == "text") {
                auto text = text_block(block, "text block");
                if (!text.empty()) parts.push_back(json::object{{"text", text}});
            } else if (type == "image")
                parts.push_back(image_part(block));
            else if (type == "document")
                document_parts(block, parts, policy_);
            else if (type == "tool_result")
                parts.push_back(tool_result(block));
            else
                invalid("user content block type " + type + " is not supported");
        }
        return parts;
    }

    json::object tool_result(const json::object &block) {
        allow(block, {"type", "tool_use_id", "content", "is_error", "cache_control"}, "tool_result block");
        auto id = string_at(required(block, "tool_use_id", "tool_result block"), "tool_result block.tool_use_id");
        auto name = names_.find(id);
        if (name == names_.end()) invalid("tool_result references an unknown tool_use_id");
        const bool error = flag(block, "is_error", "tool_result block");
        std::string text;
        json::array attachments;
        auto add_text = [&](std::string_view value) {
            if (!text.empty() && !value.empty()) text += '\n';
            text += value;
        };
        if (auto content = block.if_contains("content"); content && !content->is_null()) {
            if (content->is_string())
                text = std::string(content->as_string());
            else
                for (auto &item : array_at(*content, "tool_result block.content")) {
                    auto &inner = object_at(item, "tool_result content block");
                    const auto type = string_or(inner, "type");
                    if (type == "text")
                        add_text(text_block(inner, "tool_result text block"));
                    else if (type == "image")
                        attachments.push_back(image_part(inner));
                    else if (type == "document")
                        document_parts(inner, attachments, policy_);
                    else if (type == "tool_reference") {
                        // A client-side tool search result; every tool is already declared.
                        allow(inner, {"type", "tool_name", "name", "cache_control"}, "tool_reference block");
                        auto p = inner.if_contains("tool_name");
                        if (!p) p = inner.if_contains("name");
                        if (!p) invalid("tool_reference block.tool_name is required");
                        add_text("Tool available: " + string_at(*p, "tool_reference block.tool_name"));
                    } else
                        invalid("tool_result content block type " + type + " is not supported");
                }
        }
        const auto decoded = client_call(id);
        json::object response{{"name", name->second}, {"response", json::object{{error ? "error" : "result", text}}}};
        if (!decoded.id.empty()) response["id"] = decoded.id;
        if (!attachments.empty()) response["parts"] = std::move(attachments);
        return {{"functionResponse", std::move(response)}};
    }

    json::array assistant_parts(const json::value &content, bool keep_thinking) {
        json::array parts;
        if (content.is_string()) {
            if (!content.as_string().empty()) parts.push_back(json::object{{"text", content}});
            return parts;
        }
        auto signature = [&](std::string_view value) {
            if (auto own = own_signature(value))
                parts.push_back(json::object{{"text", ""}, {"thoughtSignature", *own}});
            else if (!value.empty())
                policy_.note("messages.content.thinking.signature");
        };
        for (auto &item : array_at(content, "assistant message content")) {
            auto &block = object_at(item, "content block");
            const auto type = string_or(block, "type");
            if (type == "text") {
                auto text = text_block(block, "text block");
                if (!text.empty()) parts.push_back(json::object{{"text", text}});
            } else if (type == "thinking") {
                allow(block, {"type", "thinking", "signature", "cache_control"}, "thinking block");
                auto thought = string_at(required(block, "thinking", "thinking block"), "thinking block.thinking");
                auto sig = block.if_contains("signature");
                auto sig_text = sig && !sig->is_null() ? string_at(*sig, "thinking block.signature") : std::string{};
                if (!keep_thinking) continue;
                if (!thought.empty()) parts.push_back(json::object{{"text", thought}, {"thought", true}});
                signature(sig_text);
            } else if (type == "redacted_thinking") {
                allow(block, {"type", "data", "cache_control"}, "redacted_thinking block");
                auto data = string_at(required(block, "data", "redacted_thinking block"), "redacted_thinking block.data");
                if (keep_thinking) signature(data);
            } else if (type == "tool_use") {
                allow(block, {"type", "id", "name", "input", "cache_control", "caller"}, "tool_use block");
                auto id = string_at(required(block, "id", "tool_use block"), "tool_use block.id");
                auto name = string_at(required(block, "name", "tool_use block"), "tool_use block.name");
                auto &input = object_at(required(block, "input", "tool_use block"), "tool_use block.input");
                if (auto caller = block.if_contains("caller"); caller && caller->is_object() &&
                                                               string_or(caller->as_object(), "type", "direct") != "direct")
                    invalid("tool_use calls from code execution are not supported");
                names_[id] = name;
                const auto decoded = client_call(id);
                json::object function{{"name", name}, {"args", input}};
                if (!decoded.id.empty()) function["id"] = decoded.id;
                json::object part{{"functionCall", std::move(function)}};
                if (decoded.signature) part["thoughtSignature"] = *decoded.signature;
                parts.push_back(std::move(part));
            } else
                invalid("assistant content block type " + type + " is not supported");
        }
        return parts;
    }

    Policy &policy_;
    bool allow_xhigh_ = false;
    std::map<std::string, std::string> names_;
    json::array pending_;
};

std::string_view stop_reason(std::string_view finish, bool calls) {
    if (finish == "MAX_TOKENS") return "max_tokens";
    if (content_filtered(finish)) return "refusal";
    return calls ? "tool_use" : "end_turn";
}

// Decodes base64 leniently (skipping other characters) to count PDF pages.
std::size_t pdf_pages(std::string_view data) {
    static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> table{};
    table.fill(-1);
    for (std::size_t i = 0; i < alphabet.size(); ++i) table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    std::string bytes;
    bytes.reserve(data.size() / 4 * 3);
    unsigned buffer = 0;
    int bits = 0;
    for (unsigned char c : data) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        const auto value = table[c];
        if (value < 0) continue;
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    // Page objects are "/Type /Page"; the page tree is "/Type /Pages".
    std::size_t pages = 0;
    for (std::size_t at = bytes.find("/Type"); at != std::string::npos; at = bytes.find("/Type", at + 5)) {
        auto next = bytes.find_first_not_of(" \t\r\n", at + 5);
        if (next != std::string::npos && bytes.compare(next, 5, "/Page") == 0 &&
            (next + 5 >= bytes.size() || !std::isalpha(static_cast<unsigned char>(bytes[next + 5]))))
            ++pages;
    }
    return std::max<std::size_t>(pages, 1);
}
std::size_t part_tokens(const json::object &part) {
    std::size_t total = 0;
    if (auto text = part.if_contains("text"); text && text->is_string()) total += count_tokens(text->as_string());
    for (auto key : {"functionCall", "functionResponse"})
        if (auto call = part.if_contains(key); call && call->is_object()) {
            auto copy = call->as_object();
            if (auto extra = copy.if_contains("parts"); extra && extra->is_array())
                for (auto &item : extra->as_array())
                    if (item.is_object()) total += part_tokens(item.as_object());
            copy.erase("parts");
            copy.erase("id");
            total += count_tokens(json::serialize(copy));
        }
    if (auto data = part.if_contains("inlineData"); data && data->is_object()) {
        // Gemini 3 default media resolution: 1120 per image, 560 per PDF page.
        const auto mime = string_or(data->as_object(), "mimeType");
        if (mime.starts_with("image/")) total += 1120;
        else if (mime == "application/pdf") total += 560 * pdf_pages(string_or(data->as_object(), "data"));
        else total += 258;
    }
    return total;
}
} // namespace

bool anthropic_thinking(const json::object &input) {
    auto thinking = input.if_contains("thinking");
    if (!thinking || !thinking->is_object()) return false;
    const auto type = string_or(thinking->as_object(), "type");
    return type == "enabled" || type == "adaptive";
}

AnthropicRequest anthropic_to_gemini(const json::object &input, bool ignore_unsupported, bool count_only,
                                     const Config &service_config) {
    AnthropicRequest out;
    Policy policy{ignore_unsupported, out.ignored};
    allow(input, {"model", "max_tokens", "messages", "system", "stop_sequences", "stream", "temperature", "top_p",
                  "top_k", "metadata", "thinking", "tools", "tool_choice", "output_config", "output_format",
                  "context_management", "cache_control", "service_tier", "container", "mcp_servers",
                  "inference_geo", "speed", "fallbacks", "diagnostics"},
          "request");
    // metadata and cache_control are hints that need no upstream field.
    for (auto key : {"service_tier", "container", "mcp_servers", "inference_geo", "speed", "fallbacks", "diagnostics"})
        if (auto p = input.if_contains(key); p && !p->is_null()) policy.unsupported(key);
    out.model = string_at(required(input, "model", "request"), "model");
    if (out.model.empty()) invalid("model cannot be empty");
    const bool allow_xhigh = supports_xhigh_thinking(out.model, service_config);
    out.stream = flag(input, "stream", "request");
    json::object generation, thinking_config;
    if (auto p = input.if_contains("max_tokens"); (p && !p->is_null()) || !count_only) {
        if (!p || !(p->is_int64() || p->is_uint64()) || (p->is_int64() && p->as_int64() < 1))
            invalid("max_tokens must be a positive integer");
        generation["maxOutputTokens"] = *p;
    }
    if (auto p = input.if_contains("temperature"); p && !p->is_null()) {
        if (!p->is_number()) invalid("temperature must be a number");
        generation["temperature"] = *p;
    }
    if (auto p = input.if_contains("top_p"); p && !p->is_null()) {
        if (!p->is_number()) invalid("top_p must be a number");
        generation["topP"] = *p;
    }
    if (auto p = input.if_contains("top_k"); p && !p->is_null()) {
        if (!(p->is_int64() || p->is_uint64())) invalid("top_k must be an integer");
        generation["topK"] = *p;
    }
    if (auto p = input.if_contains("stop_sequences"); p && !p->is_null()) {
        for (auto &item : array_at(*p, "stop_sequences")) string_at(item, "stop_sequences item");
        if (!p->as_array().empty()) generation["stopSequences"] = *p;
    }
    if (auto p = input.if_contains("thinking"); p && !p->is_null()) {
        auto &thinking = object_at(*p, "thinking");
        const auto type = string_or(thinking, "type");
        if (type == "enabled" || type == "adaptive") {
            allow(thinking, {"type", "budget_tokens", "display"}, "thinking");
            out.thinking = true;
            // Like a Gemini thinkingBudget, a budget has no verified upstream field.
            if (auto b = thinking.if_contains("budget_tokens"); b && !b->is_null())
                policy.note("thinking.budget_tokens");
            auto display = thinking.if_contains("display");
            const auto shown = display && !display->is_null() ? string_at(*display, "thinking.display") : "summarized";
            if (shown != "summarized" && shown != "omitted" && shown != "updates")
                invalid("thinking.display must be summarized, omitted or updates");
            thinking_config["includeThoughts"] = shown == "summarized";
        } else if (type == "disabled" || type == "between_tools") {
            allow(thinking, {"type"}, "thinking");
            thinking_config["includeThoughts"] = false;
        } else
            invalid("thinking.type must be enabled, adaptive, disabled or between_tools");
    } else
        thinking_config["includeThoughts"] = false;
    std::optional<std::string> effort;
    std::optional<json::value> format;
    if (auto p = input.if_contains("output_config"); p && !p->is_null()) {
        auto &config = object_at(*p, "output_config");
        allow(config, {"effort", "format", "task_budget"}, "output_config");
        if (auto e = config.if_contains("effort"); e && !e->is_null())
            effort = effort_level(*e, "output_config.effort", allow_xhigh);
        if (auto f = config.if_contains("format"); f && !f->is_null()) format = *f;
        if (auto tb = config.if_contains("task_budget"); tb && !tb->is_null())
            policy.unsupported("output_config.task_budget");
    }
    if (auto p = input.if_contains("output_format"); p && !p->is_null()) {
        if (format) invalid("output_format cannot be combined with output_config.format");
        format = *p;
    }
    if (format) {
        auto &f = object_at(*format, "output_config.format");
        allow(f, {"type", "schema"}, "output_config.format");
        if (string_or(f, "type") != "json_schema") invalid("output_config.format.type must be json_schema");
        generation["responseMimeType"] = "application/json";
        generation["responseSchema"] = object_at(required(f, "schema", "output_config.format"), "output_config.format.schema");
    }
    std::optional<std::size_t> keep_thinking;
    if (auto p = input.if_contains("context_management"); p && !p->is_null()) {
        auto &management = object_at(*p, "context_management");
        allow(management, {"edits"}, "context_management");
        if (auto edits = management.if_contains("edits"); edits && !edits->is_null())
            for (auto &item : array_at(*edits, "context_management.edits")) {
                auto &edit = object_at(item, "context_management edit");
                if (string_or(edit, "type") != "clear_thinking_20251015") {
                    policy.unsupported("context_management.edits", "only clear_thinking_20251015 is supported");
                    continue;
                }
                allow(edit, {"type", "keep"}, "clear_thinking_20251015");
                auto keep = edit.if_contains("keep");
                if (keep && keep->is_string() && keep->as_string() == "all")
                    continue;
                std::size_t turns = 1;
                if (keep && !keep->is_null()) {
                    auto &k = object_at(*keep, "clear_thinking_20251015.keep");
                    allow(k, {"type", "value"}, "clear_thinking_20251015.keep");
                    auto value = k.if_contains("value");
                    if (string_or(k, "type") != "thinking_turns" || !value || !(value->is_int64() || value->is_uint64()) ||
                        (value->is_int64() && value->as_int64() < 1))
                        invalid("clear_thinking_20251015.keep must be \"all\" or thinking_turns with a positive value");
                    turns = static_cast<std::size_t>(value->to_number<std::uint64_t>());
                }
                keep_thinking = keep_thinking ? std::min(*keep_thinking, turns) : turns;
            }
    }

    Lowering lowering(policy, allow_xhigh);
    if (auto p = input.if_contains("tools"); p && !p->is_null())
        for (auto &item : array_at(*p, "tools")) {
            auto &tool = object_at(item, "tool");
            const auto type = string_or(tool, "type");
            if (!type.empty() && type != "custom") {
                // Server tools run on Anthropic's side.
                policy.unsupported("tools." + type);
                continue;
            }
            lowering.tools.add(lowering.tools.declare(tool, policy, "tool"));
        }
    if (auto p = input.if_contains("system"); p && !p->is_null()) lowering.system(*p);
    lowering.messages(array_at(required(input, "messages", "request"), "messages"), keep_thinking);
    if (lowering.effort) effort = lowering.effort;
    if (effort) thinking_config["thinkingLevel"] = *effort;
    generation["thinkingConfig"] = std::move(thinking_config);

    json::object body{{"contents", std::move(lowering.contents_)}, {"generationConfig", std::move(generation)}};
    if (!lowering.system_.empty())
        body["systemInstruction"] = json::object{{"role", "user"}, {"parts", std::move(lowering.system_)}};
    auto declarations = lowering.tools.active();
    if (!declarations.empty()) body["tools"] = json::array{json::object{{"functionDeclarations", std::move(declarations)}}};
    if (auto p = input.if_contains("tool_choice"); p && !p->is_null()) {
        auto &choice = object_at(*p, "tool_choice");
        allow(choice, {"type", "name", "disable_parallel_tool_use"}, "tool_choice");
        const auto type = string_or(choice, "type");
        if (flag(choice, "disable_parallel_tool_use", "tool_choice")) policy.unsupported("tool_choice.disable_parallel_tool_use");
        if (type == "none")
            body["toolConfig"] = json::object{{"functionCallingConfig", json::object{{"mode", "NONE"}}}};
        else if (type == "any" || type == "tool")
            policy.unsupported("tool_choice", "forced tool use has no verified upstream mapping");
        else if (type != "auto")
            invalid("tool_choice.type must be auto, any, tool or none");
    }
    out.gemini = std::move(body);
    return out;
}

std::size_t anthropic_input_tokens(const json::object &gemini) {
    std::size_t total = 0;
    auto content_tokens = [&](const json::value &content) {
        if (!content.is_object()) return;
        if (auto parts = content.as_object().if_contains("parts"); parts && parts->is_array())
            for (auto &part : parts->as_array())
                if (part.is_object()) total += part_tokens(part.as_object());
    };
    if (auto p = gemini.if_contains("systemInstruction")) content_tokens(*p);
    if (auto p = gemini.if_contains("contents"); p && p->is_array())
        for (auto &content : p->as_array()) content_tokens(content);
    if (auto p = gemini.if_contains("tools"); p && p->is_array())
        for (auto &tool : p->as_array())
            if (tool.is_object())
                if (auto functions = tool.as_object().if_contains("functionDeclarations"); functions && functions->is_array())
                    for (auto &function : functions->as_array()) total += count_tokens(json::serialize(function));
    return total;
}

json::object anthropic_error(const Error &error) {
    std::string type = "api_error";
    switch (error.status) {
    case 400: type = "invalid_request_error"; break;
    case 401: type = "authentication_error"; break;
    case 402: type = "billing_error"; break;
    case 403: type = "permission_error"; break;
    case 404: type = "not_found_error"; break;
    case 413: type = "request_too_large"; break;
    case 429: type = "rate_limit_error"; break;
    case 503:
    case 529: type = "overloaded_error"; break;
    case 504: type = "timeout_error"; break;
    default: break;
    }
    return {{"type", "error"}, {"error", json::object{{"type", type}, {"message", error.what()}}}};
}

AnthropicStream::AnthropicStream(std::string model, std::string id, bool thinking)
    : model_(std::move(model)), id_(std::move(id)), thinking_(thinking) {}

json::object AnthropicStream::usage() const {
    auto count = [&](std::string_view key) -> std::int64_t {
        auto p = usage_.if_contains(key);
        if (!p) return 0;
        if (p->is_int64() && p->as_int64() >= 0) return p->as_int64();
        if (p->is_uint64() && p->as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
            return static_cast<std::int64_t>(p->as_uint64());
        return 0;
    };
    const auto cached = count("cachedContentTokenCount");
    return {{"input_tokens", std::max<std::int64_t>(0, count("promptTokenCount") - cached)},
            {"cache_creation_input_tokens", 0},
            {"cache_read_input_tokens", cached},
            {"output_tokens", count("candidatesTokenCount")}};
}

void AnthropicStream::open(json::array &events, Block type, json::object block) {
    close(events);
    block_ = type;
    signed_ = false;
    visible_ = visible_ || type == Block::text || type == Block::tool_use;
    events.push_back(json::object{{"type", "content_block_start"}, {"index", index_}, {"content_block", std::move(block)}});
}

void AnthropicStream::close(json::array &events) {
    if (block_ == Block::none) return;
    events.push_back(json::object{{"type", "content_block_stop"}, {"index", index_}});
    ++index_;
    block_ = Block::none;
}

void AnthropicStream::signature(json::array &events, const json::value &value) {
    if (!value.is_string() || value.as_string().empty()) return;
    events.push_back(json::object{
        {"type", "content_block_delta"},
        {"index", index_},
        {"delta", json::object{{"type", "signature_delta"}, {"signature", std::string(signature_prefix) + std::string(value.as_string())}}}});
    signed_ = true;
}

json::array AnthropicStream::push(const json::object &delta) {
    json::array events;
    if (auto usage = delta.if_contains("usageMetadata"); usage && usage->is_object()) usage_ = usage->as_object();
    if (!started_) {
        started_ = true;
        auto usage = this->usage();
        usage["output_tokens"] = 0;
        events.push_back(json::object{{"type", "message_start"},
                                      {"message", json::object{{"id", id_},
                                                               {"type", "message"},
                                                               {"role", "assistant"},
                                                               {"model", model_},
                                                               {"content", json::array{}},
                                                               {"stop_reason", nullptr},
                                                               {"stop_sequence", nullptr},
                                                               {"usage", std::move(usage)}}}});
    }
    auto candidates = delta.if_contains("candidates");
    if (!candidates || !candidates->is_array()) return events;
    for (auto &value : candidates->as_array()) {
        if (!value.is_object()) continue;
        auto &candidate = value.as_object();
        if (auto index = candidate.if_contains("index"); index && index->is_number() && json::value_to<double>(*index) != 0)
            continue;
        if (auto reason = candidate.if_contains("finishReason");
            reason && reason->is_string() && !reason->as_string().empty() &&
            reason->as_string() != "FINISH_REASON_UNSPECIFIED")
            finish_reason_ = std::string(reason->as_string());
        auto content = candidate.if_contains("content");
        auto parts = content && content->is_object() ? content->as_object().if_contains("parts") : nullptr;
        if (!parts || !parts->is_array()) continue;
        for (auto &item : parts->as_array()) {
            if (!item.is_object()) continue;
            auto &part = item.as_object();
            const auto *sig = part.if_contains("thoughtSignature");
            if (auto call = part.if_contains("functionCall"); call && call->is_object()) {
                auto &function = call->as_object();
                const auto upstream = string_or(function, "id");
                const auto local = upstream.empty()
                                       ? "call_" + sha256(id_ + ":" + json::serialize(function) + ":" + std::to_string(ordinal_)).substr(0, 24)
                                       : upstream;
                ++ordinal_;
                const auto id = signed_call_id(upstream, sig && sig->is_string() ? *sig : json::value(nullptr),
                                               sha256(id_ + ":" + local).substr(0, 16));
                open(events, Block::tool_use,
                     json::object{{"type", "tool_use"}, {"id", id}, {"name", string_or(function, "name")}, {"input", json::object{}}});
                auto args = function.if_contains("args");
                events.push_back(json::object{
                    {"type", "content_block_delta"},
                    {"index", index_},
                    {"delta", json::object{{"type", "input_json_delta"},
                                           {"partial_json", json::serialize(args && args->is_object() ? *args : json::value(json::object{}))}}}});
                close(events);
                calls_ = true;
                continue;
            }
            const bool valid_sig = sig && sig->is_string() && !sig->as_string().empty();
            const bool non_text = part.if_contains("inlineData") || part.if_contains("executableCode") ||
                                  part.if_contains("codeExecutionResult") || part.if_contains("functionResponse");
            if (non_text) {
                ++dropped_;
                continue;
            }
            auto text = part.if_contains("text");
            const bool has_text = text && text->is_string();
            if (!has_text && !valid_sig) {
                ++dropped_;
                continue;
            }
            const std::string_view chunk = has_text ? std::string_view(text->as_string()) : std::string_view{};
            if (bool_or(part, "thought")) {
                if (!chunk.empty() || valid_sig) {
                    if (block_ != Block::thinking || signed_)
                        open(events, Block::thinking, json::object{{"type", "thinking"}, {"thinking", ""}, {"signature", ""}});
                    if (!chunk.empty())
                        events.push_back(json::object{{"type", "content_block_delta"},
                                                      {"index", index_},
                                                      {"delta", json::object{{"type", "thinking_delta"}, {"thinking", chunk}}}});
                    if (valid_sig) signature(events, *sig);
                }
                continue;
            }
            if (valid_sig) {
                // The signature belongs to the turn's thinking: the open thinking
                // block, else an empty one that exists only to carry it. Gemini
                // sends a text answer's signature after the text, and a thinking
                // block there would end the message (clients such as Claude Code
                // take the last block as the answer). Such a signature closes a
                // turn, which Gemini does not validate, so it is dropped; call
                // signatures travel in the tool_use ids.
                if (block_ == Block::thinking && !signed_)
                    signature(events, *sig);
                else if (thinking_ && !visible_) {
                    open(events, Block::thinking, json::object{{"type", "thinking"}, {"thinking", ""}, {"signature", ""}});
                    signature(events, *sig);
                }
            }
            if (!chunk.empty()) {
                if (block_ != Block::text) open(events, Block::text, json::object{{"type", "text"}, {"text", ""}});
                events.push_back(json::object{{"type", "content_block_delta"},
                                              {"index", index_},
                                              {"delta", json::object{{"type", "text_delta"}, {"text", chunk}}}});
            }
        }
    }
    return events;
}

json::array AnthropicStream::finish() {
    json::array events;
    if (!started_)
        for (auto &event : push(json::object{})) events.push_back(std::move(event));
    close(events);
    events.push_back(json::object{{"type", "message_delta"},
                                  {"delta", json::object{{"stop_reason", stop_reason(finish_reason_, calls_)}, {"stop_sequence", nullptr}}},
                                  {"usage", usage()}});
    events.push_back(json::object{{"type", "message_stop"}});
    return events;
}

json::array AnthropicStream::fail(const Error &error) { return json::array{anthropic_error(error)}; }

json::object anthropic_response(const json::object &gemini, std::string_view model, std::string_view id, bool thinking,
                                std::size_t *dropped) {
    AnthropicStream stream{std::string(model), std::string(id), thinking};
    auto events = stream.push(gemini);
    for (auto &event : stream.finish()) events.push_back(std::move(event));
    if (dropped) *dropped = stream.dropped();
    json::object message;
    std::map<std::size_t, std::string> inputs;
    for (auto &value : events) {
        auto &event = value.as_object();
        const auto type = std::string(event.at("type").as_string());
        if (type == "message_start")
            message = event.at("message").as_object();
        else if (type == "content_block_start")
            message.at("content").as_array().push_back(event.at("content_block"));
        else if (type == "content_block_delta") {
            const auto index = static_cast<std::size_t>(event.at("index").to_number<std::uint64_t>());
            auto &block = message.at("content").as_array().at(index).as_object();
            auto &delta = event.at("delta").as_object();
            const auto kind = std::string(delta.at("type").as_string());
            if (kind == "text_delta") block.at("text").as_string().append(delta.at("text").as_string());
            else if (kind == "thinking_delta") block.at("thinking").as_string().append(delta.at("thinking").as_string());
            else if (kind == "signature_delta") block["signature"] = delta.at("signature");
            else if (kind == "input_json_delta") inputs[index] += std::string(delta.at("partial_json").as_string());
        } else if (type == "content_block_stop") {
            const auto index = static_cast<std::size_t>(event.at("index").to_number<std::uint64_t>());
            if (auto input = inputs.find(index); input != inputs.end())
                message.at("content").as_array().at(index).as_object()["input"] = json::parse(input->second);
        } else if (type == "message_delta") {
            for (auto &kv : event.at("delta").as_object()) message[kv.key()] = kv.value();
            message["usage"] = event.at("usage");
        }
    }
    return message;
}
} // namespace ais2api
