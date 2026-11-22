// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/responses.hpp"
#include <algorithm>
namespace ais2api {
namespace {
[[noreturn]] void bad(std::string message) {
    throw Error(400, "invalid_request_error", std::move(message));
}
bool web_search(const json::value &v) {
    if (!v.is_object()) return false;
    auto type = v.as_object().if_contains("type");
    return type && (*type == "web_search" || *type == "web_search_preview");
}
const json::object &obj(const json::value &v) {
    if (!v.is_object())
        bad("Expected an object");
    return v.as_object();
}
std::string required(const json::object &o, std::string_view k) {
    auto p = o.if_contains(k);
    if (!p || !p->is_string())
        bad(std::string(k) + " must be a string");
    return std::string(p->as_string());
}
void keys(const json::object &o, std::initializer_list<std::string_view> allowed) {
    for (auto &kv : o)
        if (std::find(allowed.begin(), allowed.end(), std::string_view(kv.key())) == allowed.end())
            bad("Unsupported Responses field: " + std::string(kv.key()));
}
json::value content(const json::value &v) {
    if (v.is_string())
        return v;
    if (!v.is_array())
        bad("Message content must be text or an array");
    json::array out;
    for (auto &x : v.as_array()) {
        auto &p = obj(x);
        auto t = required(p, "type");
        if (t == "input_text" || t == "output_text") {
            keys(p, {"type", "text", "annotations", "logprobs"});
            out.push_back(json::object{{"type", "text"}, {"text", required(p, "text")}});
        } else if (t == "input_image") {
            keys(p, {"type", "image_url", "detail"});
            auto url = required(p, "image_url");
            if (!url.starts_with("https://") && !url.starts_with("data:"))
                bad("input_image requires an HTTPS or data URL");
            json::object image{{"url", url}};
            if (auto d = p.if_contains("detail"); d && !d->is_null())
                image["detail"] = *d;
            out.push_back(json::object{{"type", "image_url"}, {"image_url", image}});
        } else
            bad("Unsupported Responses content type: " + t);
    }
    return out;
}
std::string item_text(const json::object &i) {
    auto t = string_or(i, "type");
    if (t == "function_call")
        return string_or(i, "arguments");
    auto p = i.if_contains(t == "reasoning" ? "summary" : "content");
    if (!p || !p->is_array() || p->as_array().empty())
        return {};
    return string_or(p->as_array()[0].as_object(), "text");
}
std::optional<std::string> plain_text_content(const json::object &m) {
    auto c = m.if_contains("content");
    if (!c || c->is_null())
        return std::string{};
    if (c->is_string())
        return std::string(c->as_string());
    if (!c->is_array())
        return std::nullopt;
    std::string out;
    for (auto &part : c->as_array()) {
        if (!part.is_object())
            return std::nullopt;
        auto &o = part.as_object();
        if (string_or(o, "type") != "text")
            return std::nullopt;
        auto t = o.if_contains("text");
        if (!t || !t->is_string())
            return std::nullopt;
        out.append(t->as_string());
    }
    return out;
}
json::array synthesize_assistant_parts(const json::object &m) {
    json::array parts;
    if (auto r = m.if_contains("reasoning_content"); r && r->is_string() && !r->as_string().empty())
        parts.push_back(json::object{{"text", *r}, {"thought", true}});
    if (auto text = plain_text_content(m)) {
        if (!text->empty())
            parts.push_back(json::object{{"text", *text}});
    } else if (auto c = m.if_contains("content"); c && !c->is_null()) {
        bad("Assistant message with native content must contain text only");
    }
    if (auto s = m.if_contains("thought_signature"); s && !s->is_null())
        parts.push_back(json::object{{"text", ""}, {"thoughtSignature", *s}});
    if (auto tc = m.if_contains("tool_calls"); tc && tc->is_array()) {
        for (auto &v : tc->as_array()) {
            auto &call = obj(v);
            auto &fn = obj(call.at("function"));
            auto args_str = required(fn, "arguments");
            boost::system::error_code ec;
            auto parsed = json::parse(args_str, ec);
            if (ec || !parsed.is_object())
                bad("Tool arguments must encode a JSON object");
            parts.push_back(json::object{
                {"functionCall", json::object{{"id", required(call, "id")},
                                              {"name", required(fn, "name")},
                                              {"args", std::move(parsed)}}}});
        }
    }
    return parts;
}
void append_assistant_message(json::array &messages, json::object next) {
    if (messages.empty() || string_or(messages.back().as_object(), "role") != "assistant") {
        messages.push_back(std::move(next));
        return;
    }
    auto &prev = messages.back().as_object();
    const bool prev_native = prev.if_contains("ais2api") != nullptr;
    const bool next_native = next.if_contains("ais2api") != nullptr;
    const bool dual_sig =
        prev.if_contains("thought_signature") && next.if_contains("thought_signature");
    if (prev_native || next_native || dual_sig) {
        if (!prev_native) {
            auto prev_parts = synthesize_assistant_parts(prev);
            if (auto text = plain_text_content(prev); text && prev.if_contains("content") &&
                                                      !prev.at("content").is_null())
                prev["content"] = *text;
            prev["ais2api"] = json::object{
                {"gemini_content",
                 json::object{{"role", "model"}, {"parts", std::move(prev_parts)}}}};
        }
        auto &merged_parts =
            prev.at("ais2api").as_object().at("gemini_content").as_object().at("parts").as_array();
        if (next_native) {
            auto &np = next.at("ais2api")
                           .as_object()
                           .at("gemini_content")
                           .as_object()
                           .at("parts")
                           .as_array();
            for (auto &part : np)
                merged_parts.push_back(part);
        } else {
            for (auto &part : synthesize_assistant_parts(next))
                merged_parts.push_back(std::move(part));
        }
    }
    if (auto nc = next.if_contains("content"); nc && !nc->is_null()) {
        if (auto pc = prev.if_contains("content"); !pc || pc->is_null()) {
            if (prev.if_contains("ais2api")) {
                if (auto text = plain_text_content(next))
                    prev["content"] = *text;
                else
                    prev["content"] = *nc;
            } else {
                prev["content"] = *nc;
            }
        } else if (pc->is_string() && nc->is_string()) {
            pc->as_string().append(nc->as_string());
        } else if (!prev.if_contains("ais2api") && pc->is_array() && nc->is_array()) {
            for (auto &part : nc->as_array())
                pc->as_array().push_back(part);
        } else {
            auto pt = plain_text_content(prev);
            auto nt = plain_text_content(next);
            if (!pt || !nt)
                bad("Assistant message with native content must contain text only");
            prev["content"] = *pt + *nt;
        }
    }
    if (auto nr = next.if_contains("reasoning_content");
        nr && nr->is_string() && !nr->as_string().empty()) {
        if (auto pr = prev.if_contains("reasoning_content"); pr && pr->is_string())
            pr->as_string().append(nr->as_string());
        else
            prev["reasoning_content"] = *nr;
    }
    if (auto ns = next.if_contains("thought_signature"); ns && !ns->is_null())
        prev["thought_signature"] = *ns;
    else if (prev.if_contains("thought_signature") && next_native) {
        auto &np = next.at("ais2api")
                       .as_object()
                       .at("gemini_content")
                       .as_object()
                       .at("parts")
                       .as_array();
        for (auto &part : np)
            if (part.is_object() && !part.as_object().if_contains("functionCall"))
                if (auto sig = part.as_object().if_contains("thoughtSignature"))
                    prev["thought_signature"] = *sig;
    }
    if (auto ntc = next.if_contains("tool_calls"); ntc && ntc->is_array()) {
        if (auto ptc = prev.if_contains("tool_calls"); ptc && ptc->is_array()) {
            for (auto &call : ntc->as_array())
                ptc->as_array().push_back(call);
        } else {
            prev["tool_calls"] = *ntc;
        }
    }
}
} // namespace
void discard_unsupported_responses(json::object &in, json::array &ignored) {
    auto drop = [&](std::string_view key, bool discard) {
        if (discard) {
            in.erase(key);
            ignored.push_back(json::value(key));
        }
    };
    auto one_of = [](const json::value &v, std::initializer_list<std::string_view> values) {
        return v.is_string() && std::find(values.begin(), values.end(),
            std::string_view(v.as_string())) != values.end();
    };
    for (auto key : {"store", "background"})
        if (auto p = in.if_contains(key)) drop(key, p->is_bool() && p->as_bool());
    if (auto p = in.if_contains("previous_response_id")) drop("previous_response_id", p->is_string());
    if (auto p = in.if_contains("conversation")) {
        bool valid = p->is_string();
        if (p->is_object()) {
            auto &o = p->as_object();
            auto id = o.if_contains("id");
            valid = o.size() == 1 && id && id->is_string();
        }
        drop("conversation", valid);
    }
    if (auto p = in.if_contains("include"); p && p->is_array() && !p->as_array().empty()) {
        bool valid = true;
        for (auto &v : p->as_array())
            valid = valid && one_of(v, {"code_interpreter_call.outputs", "computer_call_output.output.image_url",
                "file_search_call.results", "message.input_image.image_url", "message.output_text.logprobs",
                "reasoning.encrypted_content", "web_search_call.action.sources"});
        drop("include", valid);
    }
    if (auto p = in.if_contains("truncation")) drop("truncation", *p == "auto");
    if (auto p = in.if_contains("service_tier"))
        drop("service_tier", one_of(*p, {"default", "flex", "priority", "scale"}));
    if (auto p = in.if_contains("parallel_tool_calls"))
        drop("parallel_tool_calls", p->is_bool() && !p->as_bool());
    if (auto p = in.if_contains("reasoning"); p && p->is_object()) {
        auto &r = p->as_object();
        if (auto summary = r.if_contains("summary"); summary && one_of(*summary, {"auto", "concise", "detailed"})) {
            r.erase("summary");
            ignored.push_back("reasoning.summary");
        }
    }
    auto discard_strict = [&](json::object &o, std::string_view path) {
        if (auto strict = o.if_contains("strict"); strict && strict->is_bool() && strict->as_bool()) {
            o.erase("strict");
            ignored.push_back(json::value(path));
        }
    };
    if (auto tools = in.if_contains("tools"); tools && tools->is_array())
        for (auto &tool : tools->as_array())
            if (tool.is_object()) {
                auto &o = tool.as_object();
                if (auto type = o.if_contains("type"); type && *type == "function")
                    discard_strict(o, "responses.tools.function.strict");
            }
    if (auto text = in.if_contains("text"); text && text->is_object())
        if (auto format = text->as_object().if_contains("format"); format && format->is_object()) {
            auto &o = format->as_object();
            if (auto type = o.if_contains("type"); type && *type == "json_schema")
                discard_strict(o, "responses.text.format.strict");
        }
    auto strings = [](const json::value &v) {
        if (!v.is_array()) return false;
        return std::all_of(v.as_array().begin(), v.as_array().end(),
                           [](const json::value &entry) { return entry.is_string(); });
    };
    auto named_tools = [&](const json::value &v, bool read_only) {
        if (!v.is_object()) return false;
        auto &o = v.as_object();
        auto names = o.if_contains("tool_names");
        if (!names || !strings(*names)) return false;
        for (auto &entry : o)
            if (entry.key() != "tool_names" &&
                !(read_only && entry.key() == "read_only" && entry.value().is_bool())) return false;
        return true;
    };
    auto container = [&](const json::value &v) {
        if (v.is_string()) return true;
        if (!v.is_object()) return false;
        auto &o = v.as_object();
        auto type = o.if_contains("type");
        if (!type || *type != "auto") return false;
        for (auto &entry : o) {
            if (entry.key() == "type") continue;
            if (entry.key() == "file_ids" && strings(entry.value())) continue;
            if (entry.key() == "memory_limit" && one_of(entry.value(), {"1g", "4g", "16g", "64g"})) continue;
            return false;
        }
        return true;
    };
    auto location = [](const json::value &v) {
        if (!v.is_object()) return false;
        auto &o = v.as_object();
        auto type = o.if_contains("type");
        if (!type || *type != "approximate") return false;
        for (auto &entry : o)
            if (entry.key() != "type" &&
                !((entry.key() == "city" || entry.key() == "country" || entry.key() == "region" ||
                   entry.key() == "timezone") && entry.value().is_string())) return false;
        return true;
    };
    // Keep unknown fields and malformed definitions so the normal validator still rejects them.
    auto builtin = [&](const json::value &v, bool choice) {
        if (!v.is_object()) return false;
        auto &o = v.as_object();
        auto type = o.if_contains("type");
        if (!type || !one_of(*type, {"web_search", "web_search_preview", "file_search", "code_interpreter",
                "computer_use_preview", "image_generation", "mcp", "local_shell", "shell", "apply_patch"})) return false;
        if (!choice) {
            auto required_string = [&](std::string_view key) {
                auto p = o.if_contains(key);
                return p && p->is_string();
            };
            if (*type == "mcp" && !required_string("server_label")) return false;
            if (*type == "file_search" && !o.if_contains("vector_store_ids")) return false;
            if (*type == "code_interpreter") {
                auto c = o.if_contains("container");
                if (!c || !container(*c)) return false;
            }
            if (*type == "computer_use_preview" &&
                (!o.if_contains("display_width") || !o.if_contains("display_height") || !required_string("environment"))) return false;
        }
        for (auto &kv : o) {
            if (kv.key() == "type") continue;
            if (choice) {
                if ((kv.key() != "server_label" && kv.key() != "name") || !kv.value().is_string()) return false;
                continue;
            }
            if (*type == "code_interpreter" && kv.key() == "container" && container(kv.value())) continue;
            if ((*type == "web_search" || *type == "web_search_preview") && kv.key() == "user_location" && location(kv.value())) continue;
            if (*type == "web_search" && kv.key() == "filters" && kv.value().is_object()) {
                auto &filters = kv.value().as_object();
                auto domains = filters.if_contains("allowed_domains");
                if (filters.size() == 1 && domains && strings(*domains)) continue;
            }
            if (*type == "mcp") {
                if (kv.key() == "headers" && kv.value().is_object() &&
                    std::all_of(kv.value().as_object().begin(), kv.value().as_object().end(),
                        [](const auto &header) { return header.value().is_string(); })) continue;
                if (kv.key() == "allowed_tools" && (strings(kv.value()) || named_tools(kv.value(), true))) continue;
                if (kv.key() == "require_approval") {
                    if (one_of(kv.value(), {"always", "never"})) continue;
                    if (kv.value().is_object() && !kv.value().as_object().empty()) {
                        bool valid = true;
                        for (auto &rule : kv.value().as_object())
                            valid = valid && (rule.key() == "always" || rule.key() == "never") && named_tools(rule.value(), false);
                        if (valid) continue;
                    }
                }
            }
            // Only discard definitions whose optional configuration is understood here.
            if ((kv.key() == "container" || kv.key() == "search_context_size" || kv.key() == "server_label" || kv.key() == "server_url" ||
                 kv.key() == "server_description" || kv.key() == "authorization" || kv.key() == "environment" ||
                 kv.key() == "model" || kv.key() == "size" || kv.key() == "quality" || kv.key() == "background" ||
                 kv.key() == "output_format" || kv.key() == "moderation") && kv.value().is_string()) continue;
            if ((kv.key() == "vector_store_ids" || kv.key() == "allowed_domains") && kv.value().is_array()) {
                bool valid = true;
                for (auto &entry : kv.value().as_array()) valid = valid && entry.is_string();
                if (valid) continue;
            }
            if ((kv.key() == "display_width" || kv.key() == "display_height" || kv.key() == "max_num_results" ||
                 kv.key() == "partial_images" || kv.key() == "output_compression") && kv.value().is_int64() &&
                 kv.value().as_int64() > 0) continue;
            return false;
        }
        return true;
    };
    if (auto p = in.if_contains("tools"); p && p->is_array()) {
        auto &a = p->as_array();
        // Web search maps to Google Search instead of being discarded.
        for (std::size_t i = a.size(); i-- > 0;)
            if (builtin(a[i], false) && !web_search(a[i])) {
                ignored.push_back(json::value("tools[" + std::to_string(i) + "]"));
                a.erase(a.begin() + i);
            }
    }
    if (auto p = in.if_contains("tool_choice")) {
        bool forced = *p == "required";
        if (p->is_object()) {
            auto &o = p->as_object();
            auto type = o.if_contains("type");
            auto name = o.if_contains("name");
            forced = o.size() == 2 && type && *type == "function" && name && name->is_string() && !name->as_string().empty();
        }
        drop("tool_choice", forced || builtin(*p, true));
    }
}
json::object responses_to_chat(const json::object &in) {
    keys(in, {"model",
              "input",
              "instructions",
              "stream",
              "store",
              "background",
              "previous_response_id",
              "conversation",
              "tools",
              "tool_choice",
              "parallel_tool_calls",
              "max_output_tokens",
              "reasoning",
              "text",
              "temperature",
              "top_p",
              "metadata",
              "user",
              "ais2api",
              "include",
              "truncation",
              "service_tier"});
    for (auto k : {"store", "background"})
        if (auto p = in.if_contains(k); p && !p->is_null() && (!p->is_bool() || p->as_bool()))
            bad(std::string(k) + "=true is unsupported");
    for (auto k : {"previous_response_id", "conversation"})
        if (auto p = in.if_contains(k); p && !p->is_null())
            bad(std::string(k) + " is unsupported; provide full input history");
    if (auto p = in.if_contains("include");
        p && !p->is_null() && (!p->is_array() || !p->as_array().empty()))
        bad("include is unsupported");
    if (auto p = in.if_contains("truncation"); p && !p->is_null() && *p != "disabled")
        bad("Only disabled truncation is supported");
    if (auto p = in.if_contains("service_tier"); p && !p->is_null() && *p != "auto")
        bad("Only auto service_tier is supported");
    if (auto p = in.if_contains("metadata"); p && !p->is_null()) {
        if (!p->is_object())
            bad("metadata must be an object of strings");
        if (p->as_object().size() > 16)
            bad("metadata supports at most 16 entries");
        for (auto &kv : p->as_object())
            if (kv.key().size() > 64 || !kv.value().is_string() ||
                kv.value().as_string().size() > 512)
                bad("metadata keys and values must be strings within Responses limits");
    }
    if (auto p = in.if_contains("parallel_tool_calls");
        p && !p->is_null() && (!p->is_bool() || !p->as_bool()))
        bad("Only parallel_tool_calls=true is supported");
    json::object out{{"model", required(in, "model")}, {"store", false}};
    json::array messages;
    if (auto p = in.if_contains("instructions"); p && !p->is_null())
        messages.push_back(
            json::object{{"role", "system"}, {"content", required(in, "instructions")}});
    auto input = in.if_contains("input");
    if (!input)
        bad("input is required");
    if (input->is_string())
        messages.push_back(json::object{{"role", "user"}, {"content", *input}});
    else {
        if (!input->is_array())
            bad("input must be a string or array");
        for (auto &v : input->as_array()) {
            auto &i = obj(v);
            auto type = string_or(i, "type", "message");
            if (type == "message") {
                keys(i, {"type", "id", "status", "role", "content", "ais2api", "thought_signature",
                         "reasoning_content"});
                auto role = required(i, "role");
                if (role != "user" && role != "assistant" && role != "system" &&
                    role != "developer")
                    bad("Unsupported Responses message role");
                json::object m{{"role", role}};
                auto c = i.if_contains("content");
                if (!c)
                    bad("message content is required");
                m["content"] = content(*c);
                if (auto ext = i.if_contains("ais2api"); ext && !ext->is_null()) {
                    std::string joined;
                    if (m.at("content").is_array())
                        for (auto &part : m.at("content").as_array())
                            joined += required(obj(part), "text");
                    else
                        joined = required(i, "content");
                    m["content"] = joined;
                }
                for (auto k : {"ais2api", "thought_signature", "reasoning_content"})
                    if (auto p = i.if_contains(k); p && !p->is_null())
                        m[k] = *p;
                if (role == "assistant")
                    append_assistant_message(messages, std::move(m));
                else
                    messages.push_back(std::move(m));
            } else if (type == "function_call") {
                keys(i, {"type", "id", "status", "call_id", "name", "arguments"});
                json::object function{{"name", required(i, "name")},
                                      {"arguments", required(i, "arguments")}};
                json::object call{
                    {"id", required(i, "call_id")}, {"type", "function"}, {"function", function}};
                append_assistant_message(
                    messages,
                    json::object{{"role", "assistant"}, {"tool_calls", json::array{call}}});
            } else if (type == "function_call_output") {
                keys(i, {"type", "id", "status", "call_id", "output"});
                messages.push_back(json::object{{"role", "tool"},
                                                {"tool_call_id", required(i, "call_id")},
                                                {"content", required(i, "output")}});
            } else if (type == "reasoning") {
                keys(i, {"type", "id", "status", "summary", "thought_signature", "ais2api"});
                std::string text;
                if (auto p = i.if_contains("summary"); p && !p->is_null()) {
                    if (!p->is_array())
                        bad("reasoning summary must be an array");
                    for (auto &s : p->as_array()) {
                        auto &part = obj(s);
                        if (required(part, "type") != "summary_text")
                            bad("Unsupported reasoning summary");
                        text += required(part, "text");
                    }
                }
                json::object m{{"role", "assistant"}, {"reasoning_content", text}};
                if (auto ext = i.if_contains("ais2api"); ext && !ext->is_null())
                    m["ais2api"] = *ext;
                if (auto p = i.if_contains("thought_signature"); p && !p->is_null())
                    m["thought_signature"] = *p;
                if (!text.empty() || m.if_contains("thought_signature") || m.if_contains("ais2api"))
                    append_assistant_message(messages, std::move(m));
            } else
                bad("Unsupported Responses input item: " + type);
        }
    }
    out["messages"] = messages;
    for (auto k :
         {"stream", "temperature", "top_p", "parallel_tool_calls", "metadata", "user", "ais2api"})
        if (auto p = in.if_contains(k); p && !p->is_null())
            out[k] = *p;
    if (auto p = in.if_contains("max_output_tokens"); p && !p->is_null())
        out["max_completion_tokens"] = *p;
    if (auto p = in.if_contains("reasoning"); p && !p->is_null()) {
        auto &r = obj(*p);
        keys(r, {"effort", "summary"});
        if (auto s = r.if_contains("summary"); s && !s->is_null())
            bad("Requested reasoning summaries are unsupported");
        if (auto e = r.if_contains("effort"); e && !e->is_null())
            out["reasoning_effort"] = *e;
    }
    if (auto p = in.if_contains("text"); p && !p->is_null()) {
        auto &t = obj(*p);
        keys(t, {"format"});
        if (auto f = t.if_contains("format"); f && !f->is_null()) {
            auto format = obj(*f);
            auto type = required(format, "type");
            if (type == "json_schema") {
                format.erase("type");
                out["response_format"] = json::object{{"type", type}, {"json_schema", format}};
            } else {
                keys(format, {"type"});
                out["response_format"] = format;
            }
        }
    }
    if (auto p = in.if_contains("tools"); p && !p->is_null()) {
        if (!p->is_array())
            bad("tools must be an array");
        json::array tools;
        for (auto &v : p->as_array()) {
            auto f = obj(v);
            if (web_search(v)) {
                keys(f, {"type", "user_location", "filters", "search_context_size"});
                if (out.if_contains("web_search_options"))
                    bad("Only one web_search tool is supported");
                json::object options;
                if (auto size = f.if_contains("search_context_size"); size && !size->is_null()) {
                    if (!(*size == "low" || *size == "medium" || *size == "high"))
                        bad("search_context_size must be low, medium or high");
                    options["search_context_size"] = *size;
                }
                // Chat Completions nests the approximate location fields.
                if (auto loc = f.if_contains("user_location"); loc && !loc->is_null()) {
                    auto location = obj(*loc);
                    keys(location, {"type", "city", "country", "region", "timezone"});
                    if (required(location, "type") != "approximate")
                        bad("user_location.type must be approximate");
                    location.erase("type");
                    for (auto it = location.begin(); it != location.end();) {
                        if (it->value().is_null())
                            it = location.erase(it);
                        else {
                            if (!it->value().is_string())
                                bad("user_location fields must be strings");
                            ++it;
                        }
                    }
                    options["user_location"] =
                        json::object{{"type", "approximate"}, {"approximate", location}};
                }
                if (auto flt = f.if_contains("filters"); flt && !flt->is_null()) {
                    auto filters = obj(*flt);
                    keys(filters, {"allowed_domains"});
                    if (auto domains = filters.if_contains("allowed_domains");
                        domains && !domains->is_null()) {
                        if (!domains->is_array()) bad("allowed_domains must be an array");
                        for (auto &domain : domains->as_array())
                            if (!domain.is_string()) bad("allowed_domains must contain strings");
                    } else {
                        filters.erase("allowed_domains");
                    }
                    options["filters"] = filters;
                }
                out["web_search_options"] = std::move(options);
                continue;
            }
            if (required(f, "type") != "function")
                bad("Only function and web_search tools are supported");
            keys(f, {"type", "name", "description", "parameters", "strict"});
            f.erase("type");
            for (auto optional : {"description", "parameters", "strict"})
                if (auto v = f.if_contains(optional); v && v->is_null())
                    f.erase(optional);
            tools.push_back(json::object{{"type", "function"}, {"function", f}});
        }
        if (!tools.empty() || !out.if_contains("web_search_options"))
            out["tools"] = tools;
    }
    if (auto p = in.if_contains("tool_choice"); p && !p->is_null()) {
        if (p->is_object()) {
            auto &t = p->as_object();
            keys(t, {"type", "name"});
            if (required(t, "type") != "function")
                bad("Only function tool_choice is supported");
            out["tool_choice"] = json::object{
                {"type", "function"}, {"function", json::object{{"name", required(t, "name")}}}};
        } else
            out["tool_choice"] = *p;
    }
    return out;
}
json::object responses_options(const json::object &in) {
    json::object out;
    for (auto k : {"instructions", "user", "temperature", "top_p", "parallel_tool_calls",
                   "max_output_tokens", "reasoning", "text", "metadata", "tools", "tool_choice"})
        if (auto p = in.if_contains(k); p && !p->is_null()) {
            if ((std::string_view(k) == "reasoning" || std::string_view(k) == "text") &&
                p->is_object()) {
                json::object filtered;
                for (auto &kv : p->as_object())
                    if (!kv.value().is_null())
                        filtered[kv.key()] = kv.value();
                if (!filtered.empty())
                    out[k] = std::move(filtered);
            } else {
                out[k] = *p;
            }
        }
    return out;
}
json::object responses_response(const json::object &gemini, std::string_view model,
                                std::string_view id, const json::object &options, Store *store,
                                std::string_view principal) {
    auto chat = openai_response(gemini, model, id, store, principal);
    json::array output;
    std::string status = "completed";
    json::value incomplete = nullptr;
    if (!chat.at("choices").as_array().empty()) {
        auto &choice = chat.at("choices").as_array()[0].as_object();
        auto &m = choice.at("message").as_object();
        json::array native_text, native_reasoning;
        if (auto ext = m.if_contains("ais2api"))
            for (auto &part :
                 ext->as_object().at("gemini_content").as_object().at("parts").as_array())
                if (!part.as_object().if_contains("functionCall") &&
                    !bool_or(part.as_object(), "thought"))
                    native_text.push_back(part);
                else if (bool_or(part.as_object(), "thought"))
                    native_reasoning.push_back(part);
        std::optional<json::value> reasoning_signature;
        for (auto &part : native_reasoning)
            if (auto sig = part.as_object().if_contains("thoughtSignature"))
                reasoning_signature = *sig;
        auto reason = string_or(choice, "finish_reason");
        if (reason == "length" || reason == "content_filter") {
            status = "incomplete";
            incomplete = json::object{
                {"reason", reason == "length" ? "max_output_tokens" : "content_filter"}};
        }
        if (auto p = m.if_contains("reasoning_content");
            (p && p->is_string() && !p->as_string().empty()) || reasoning_signature) {
            json::object item{
                {"id", std::string(id) + "_reasoning"},
                {"type", "reasoning"},
                {"status", status},
                {"summary",
                 json::array{json::object{
                     {"type", "summary_text"},
                     {"text", p && p->is_string() ? *p : json::value("")}}}}};
            if (reasoning_signature)
                item["thought_signature"] = *reasoning_signature;
            if (!native_reasoning.empty())
                item["ais2api"] =
                    json::object{{"gemini_content",
                                  json::object{{"role", "model"}, {"parts", native_reasoning}}}};
            output.push_back(item);
        }
        if (auto p = m.if_contains("content"); (p && p->is_string() && !p->as_string().empty()) ||
                                               m.if_contains("thought_signature") ||
                                               !native_text.empty()) {
            json::object item{{"id", std::string(id) + "_message"},
                              {"type", "message"},
                              {"role", "assistant"},
                              {"status", status},
                              {"content", json::array{json::object{
                                              {"type", "output_text"},
                                              {"text", p && p->is_string() ? *p : json::value("")},
                                              {"annotations", json::array{}}}}}};
            if (auto signature = m.if_contains("thought_signature")) {
                if (native_text.empty())
                    item["thought_signature"] = *signature;
                else
                    for (auto &part : native_text)
                        if (auto sig = part.as_object().if_contains("thoughtSignature"))
                            item["thought_signature"] = *sig;
            }
            if (!native_text.empty())
                item["ais2api"] = json::object{
                    {"gemini_content", json::object{{"role", "model"}, {"parts", native_text}}}};
            output.push_back(item);
        }
        if (auto p = m.if_contains("tool_calls")) {
            json::object seen_calls;
            std::size_t call_index = 0;
            for (auto &v : p->as_array()) {
                auto &c = v.as_object();
                auto &f = c.at("function").as_object();
                auto call_id = string_or(c, "id");
                if (auto seen = seen_calls.if_contains(call_id)) {
                    if (*seen != f)
                        throw Error(502, "upstream_protocol_error",
                                    "Repeated function call ID has conflicting arguments");
                    continue;
                }
                seen_calls[call_id] = f;
                output.push_back(
                    json::object{{"id", std::string(id) + "_fc_" + std::to_string(call_index++)},
                                 {"type", "function_call"},
                                 {"status", status},
                                 {"call_id", c.at("id")},
                                 {"name", f.at("name")},
                                 {"arguments", f.at("arguments")}});
            }
        }
    }
    json::object result{{"tools", json::array{}},
                        {"tool_choice", "auto"},
                        {"parallel_tool_calls", true},
                        {"instructions", nullptr},
                        {"max_output_tokens", nullptr},
                        {"text", json::object{{"format", json::object{{"type", "text"}}}}},
                        {"reasoning", json::object{{"effort", nullptr}, {"summary", nullptr}}},
                        {"temperature", 1.0},
                        {"top_p", 1.0},
                        {"metadata", json::object{}},
                        {"truncation", "disabled"},
                        {"user", nullptr}};
    for (auto &kv : options) {
        if ((kv.key() == "reasoning" || kv.key() == "text") && kv.value().is_object()) {
            for (auto &nested : kv.value().as_object())
                result.at(kv.key()).as_object()[nested.key()] = nested.value();
        } else
            result[kv.key()] = kv.value();
    }
    result["id"] = id;
    result["object"] = "response";
    result["created_at"] = unix_seconds();
    result["model"] = model;
    result["status"] = status;
    result["output"] = output;
    result["error"] = nullptr;
    result["incomplete_details"] = incomplete;
    result["store"] = false;
    result["background"] = false;
    result["previous_response_id"] = nullptr;
    result["usage"] = nullptr;
    if (auto u = chat.if_contains("usage")) {
        auto &x = u->as_object();
        json::object usage{{"input_tokens", x.at("prompt_tokens")},
                           {"output_tokens", x.at("completion_tokens")},
                           {"total_tokens", x.at("total_tokens")},
                           {"input_tokens_details", json::object{{"cached_tokens", 0}}},
                           {"output_tokens_details", json::object{{"reasoning_tokens", 0}}}};
        if (auto d = x.if_contains("prompt_tokens_details"))
            usage["input_tokens_details"] = *d;
        if (auto d = x.if_contains("completion_tokens_details"))
            usage["output_tokens_details"] = *d;
        result["usage"] = usage;
    }
    return result;
}
ResponsesStream::ResponsesStream(std::string model, std::string id, json::object options,
                                 std::size_t limit, Store *store, std::string principal)
    : model_(std::move(model)), id_(std::move(id)), principal_(std::move(principal)),
      options_(std::move(options)), limit_(limit), created_(unix_seconds()), store_(store) {
    if (!limit_)
        bad("Responses stream limit must be positive");
    options_bytes_ = json_size(options_);
    if (options_bytes_ > limit_)
        throw Error(413, "response_too_large", "Responses options exceed configured object limit");
}
json::object ResponsesStream::event(std::string_view type, json::object fields) {
    fields["type"] = type;
    fields["sequence_number"] = sequence_++;
    return fields;
}
json::object ResponsesStream::response(bool persist) {
    auto r =
        responses_response(aggregate_, model_, id_, options_, persist ? store_ : nullptr, principal_);
    r["created_at"] = created_;
    return r;
}
json::array ResponsesStream::start() {
    if (started_ || ended_)
        return {};
    started_ = true;
    auto r = response();
    r["status"] = "in_progress";
    return {event("response.created", {{"response", r}}),
            event("response.in_progress", {{"response", r}})};
}
namespace {
std::int64_t candidate_index(const json::object &candidate) {
    auto index = candidate.if_contains("index");
    if (!index) return 0;
    if (index->is_int64()) return index->as_int64();
    if (index->is_uint64() && index->as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(index->as_uint64());
    return 0;
}
} // namespace
const json::array *ResponsesStream::first_parts(const json::object &aggregate) {
    auto candidates = aggregate.if_contains("candidates");
    if (!candidates || !candidates->is_array() || candidates->as_array().empty() ||
        !candidates->as_array()[0].is_object())
        return nullptr;
    auto content = candidates->as_array()[0].as_object().if_contains("content");
    if (!content || !content->is_object()) return nullptr;
    auto parts = content->as_object().if_contains("parts");
    return parts && parts->is_array() ? &parts->as_array() : nullptr;
}
// A delta that only carries one plain text part for the first candidate, which
// merge_event joins onto that candidate's last part. The Responses output reads
// nothing from a candidate but its index, content and finish reason, and a
// finish reason only changes the final status, so the join changes exactly one
// item: its text and its last native part grow by the text. Everything is
// checked before merging; once planned, merging and applying cannot fail.
std::optional<ResponsesStream::Append> ResponsesStream::plan_append(const json::object &delta) const {
    auto incoming = delta.if_contains("candidates");
    if (!incoming || !incoming->is_array() || incoming->as_array().size() != 1 ||
        !incoming->as_array()[0].is_object())
        return {};
    const auto &candidate = incoming->as_array()[0].as_object();
    const auto *parts = first_parts(aggregate_);
    if (!parts || parts->empty() || !parts->back().is_object() ||
        candidate_index(candidate) != candidate_index(aggregate_.at("candidates").as_array()[0].as_object()))
        return {};
    auto content = candidate.if_contains("content");
    if (!content || !content->is_object()) return {};
    auto incoming_parts = content->as_object().if_contains("parts");
    if (!incoming_parts || !incoming_parts->is_array() || incoming_parts->as_array().size() != 1 ||
        !incoming_parts->as_array()[0].is_object())
        return {};
    const auto &part = incoming_parts->as_array()[0].as_object();
    const auto &last = parts->back().as_object();
    if (!joins_text(last, part) || (part.if_contains("thought") && !part.at("thought").is_bool()))
        return {};
    Append plan{std::string_view(part.at("text").as_string()), bool_or(part, "thought")};
    const auto id = id_ + (plan.thought ? "_reasoning" : "_message");
    while (plan.index < output_.size() && string_or(output_[plan.index].as_object(), "id") != id) ++plan.index;
    if (plan.index == output_.size()) return {};
    const auto &item = output_[plan.index].as_object();
    auto list = item.if_contains(plan.thought ? "summary" : "content");
    if (!list || !list->is_array() || list->as_array().empty() || !list->as_array()[0].is_object()) return {};
    auto text = list->as_array()[0].as_object().if_contains("text");
    if (!text || !text->is_string()) return {};
    if (auto ext = item.if_contains("ais2api")) {
        // The item's last native part must be the part this delta extends.
        const auto &native = ext->as_object().at("gemini_content").as_object().at("parts").as_array();
        if (native.empty() || !native.back().is_object()) return {};
        auto native_text = native.back().as_object().if_contains("text");
        if (!native_text || !native_text->is_string() ||
            native_text->as_string().size() != last.at("text").as_string().size())
            return {};
        plan.native = true;
    }
    plan.growth = 2 * json::serialize(json::value(plan.text)).size();
    const auto merge_growth = merge_growth_bound(delta, 1, 1);
    if (aggregate_bound_ > limit_ || merge_growth > limit_ - aggregate_bound_ ||
        aggregate_bound_ + merge_growth + output_bound_ + plan.growth + options_bytes_ > limit_)
        return {};
    return plan;
}
void ResponsesStream::apply_append(const Append &plan, json::array &events) {
    auto &item = output_[plan.index].as_object();
    const auto id = string_or(item, "id");
    if (!plan.text.empty()) {
        item.at(plan.thought ? "summary" : "content").as_array()[0].as_object().at("text").as_string().append(plan.text);
        if (plan.native)
            item.at("ais2api").as_object().at("gemini_content").as_object().at("parts").as_array()
                .back().as_object().at("text").as_string().append(plan.text);
    }
    output_bound_ += plan.growth;
    if (plan.text.empty()) return;
    json::object fields{{"item_id", id}, {"output_index", plan.index}, {"delta", plan.text}};
    fields[plan.thought ? "summary_index" : "content_index"] = 0;
    events.push_back(event(plan.thought ? "response.reasoning_summary_text.delta"
                                        : "response.output_text.delta",
                           fields));
}
json::array ResponsesStream::push(const json::object &delta) {
    if (ended_)
        return {};
    const bool prior_started = started_;
    const auto prior_sequence = sequence_;
    const auto prior_output_bound = output_bound_;
    auto events = start();
    if (incremental)
        if (auto plan = plan_append(delta)) {
            merge_event(aggregate_, delta, limit_, &aggregate_bound_);
            apply_append(*plan, events);
            return events;
        }
    // Undo log for output_: appended count and replaced items. Conversion can
    // reject the merged aggregate, and fail() converts it again.
    auto prior_aggregate = aggregate_;
    const auto prior_bound = aggregate_bound_;
    const auto prior_size = output_.size();
    std::vector<std::pair<std::size_t, json::value>> replaced;
    try {
        merge_event(aggregate_, delta, limit_, &aggregate_bound_);
        auto r = response();
        auto &next = r.at("output").as_array();
        // Preserve discovery order even if reasoning arrives after visible output.
        for (auto &v : next) {
            auto item = std::move(v.as_object());
            auto id = string_or(item, "id");
            std::size_t index = 0;
            for (; index < output_.size(); ++index)
                if (string_or(output_[index].as_object(), "id") == id)
                    break;
            auto type = string_or(item, "type");
            auto text = item_text(item);
            std::string previous;
            if (index == output_.size()) {
                auto initial = item;
                initial["status"] = "in_progress";
                if (type == "function_call")
                    initial["arguments"] = "";
                else {
                    auto key = type == "reasoning" ? "summary" : "content";
                    initial[key].as_array()[0].as_object()["text"] = "";
                }
                events.push_back(event("response.output_item.added",
                                       {{"output_index", index}, {"item", initial}}));
                output_.push_back(initial);
                if (type != "function_call") {
                    auto part =
                        initial.at(type == "reasoning" ? "summary" : "content").as_array()[0];
                    events.push_back(
                        event(type == "reasoning" ? "response.reasoning_summary_part.added"
                                                  : "response.content_part.added",
                              {{"item_id", id},
                               {"output_index", index},
                               {type == "reasoning" ? "summary_index" : "content_index", 0},
                               {"part", part}}));
                }
            } else
                previous = item_text(output_[index].as_object());
            if (!text.starts_with(previous))
                throw Error(502, "upstream_protocol_error",
                            "Responses output was revised by upstream");
            if (text.size() > previous.size()) {
                json::object fields{{"item_id", id},
                                    {"output_index", index},
                                    {"delta", text.substr(previous.size())}};
                if (type != "function_call")
                    fields[type == "reasoning" ? "summary_index" : "content_index"] = 0;
                events.push_back(
                    event(type == "function_call" ? "response.function_call_arguments.delta"
                          : type == "reasoning"   ? "response.reasoning_summary_text.delta"
                                                  : "response.output_text.delta",
                          fields));
            }
            item["status"] = "in_progress";
            if (index < prior_size) replaced.emplace_back(index, std::move(output_[index]));
            output_[index] = std::move(item);
        }
        // The aggregate share uses merge_event's upper bound; measure exactly
        // only when that bound could cross the limit.
        output_bound_ = json_size(output_);
        if (aggregate_bound_ + output_bound_ + options_bytes_ > limit_) {
            aggregate_bound_ = json_size(aggregate_);
            if (aggregate_bound_ + output_bound_ + options_bytes_ > limit_)
                throw Error(413, "response_too_large",
                            "Responses stream exceeds configured object limit");
        }
        return events;
    } catch (...) {
        aggregate_ = std::move(prior_aggregate);
        aggregate_bound_ = prior_bound;
        for (auto &[index, item] : replaced) output_[index] = std::move(item);
        output_.resize(prior_size);
        started_ = prior_started;
        sequence_ = prior_sequence;
        output_bound_ = prior_output_bound;
        throw;
    }
}
json::array ResponsesStream::finish() {
    if (ended_)
        return {};
    auto events = start();
    auto r = response(true);
    for (std::size_t i = 0; i < output_.size(); ++i) {
        auto &item = output_[i].as_object();
        item["status"] = r.at("status");
        auto t = string_or(item, "type");
        auto id = string_or(item, "id");
        json::object fields{{"item_id", id},
                            {"output_index", i},
                            {t == "function_call" ? "arguments" : "text", item_text(item)}};
        if (t != "function_call")
            fields[t == "reasoning" ? "summary_index" : "content_index"] = 0;
        events.push_back(event(t == "function_call" ? "response.function_call_arguments.done"
                               : t == "reasoning"   ? "response.reasoning_summary_text.done"
                                                    : "response.output_text.done",
                               fields));
        if (t != "function_call")
            events.push_back(
                event(t == "reasoning" ? "response.reasoning_summary_part.done"
                                       : "response.content_part.done",
                      {{"item_id", id},
                       {"output_index", i},
                       {t == "reasoning" ? "summary_index" : "content_index", 0},
                       {"part", item.at(t == "reasoning" ? "summary" : "content").as_array()[0]}}));
        events.push_back(event("response.output_item.done", {{"output_index", i}, {"item", item}}));
    }
    r["output"] = output_;
    events.push_back(
        event(string_or(r, "status") == "incomplete" ? "response.incomplete" : "response.completed",
              {{"response", r}}));
    ended_ = true;
    return events;
}
json::array ResponsesStream::fail(const Error &e) {
    if (ended_)
        return {};
    auto events = start();
    auto r = response();
    r["status"] = "failed";
    r["error"] = json::object{{"code", e.code}, {"message", e.what()}};
    for (auto &item : output_)
        item.as_object()["status"] = "incomplete";
    r["output"] = output_;
    events.push_back(event("response.failed", {{"response", r}}));
    ended_ = true;
    return events;
}
} // namespace ais2api
