// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The host's URL interface on ada, a WHATWG URL parser. After AIStudio2API's
// internal/waa/url.go (MIT).
#include "text.hpp"
#include <ada.h>

namespace ais2api::waa {
namespace {
std::string url_record(const ada::url_aggregator &url) {
    return sorted_json(json::object{
        {"href", url.get_href()}, {"origin", url.get_origin()}, {"protocol", url.get_protocol()},
        {"username", url.get_username()}, {"password", url.get_password()}, {"host", url.get_host()},
        {"hostname", url.get_hostname()}, {"port", url.get_port()}, {"pathname", url.get_pathname()},
        {"search", url.get_search()}, {"hash", url.get_hash()}});
}
} // namespace

std::optional<std::string> parse_url(std::string_view input, const std::string *base) {
    if (!base) {
        const auto url = ada::parse<ada::url_aggregator>(input);
        if (!url) return std::nullopt;
        return url_record(*url);
    }
    const auto parent = ada::parse<ada::url_aggregator>(*base);
    if (!parent) return std::nullopt;
    const auto url = ada::parse<ada::url_aggregator>(input, &*parent);
    if (!url) return std::nullopt;
    return url_record(*url);
}

std::optional<std::string> update_url(std::string_view href, std::string_view field, std::string_view value) {
    auto url = ada::parse<ada::url_aggregator>(href);
    if (!url) return std::nullopt;
    if (field == "href") {
        url = ada::parse<ada::url_aggregator>(value);
        if (!url) return std::nullopt;
    } else if (field == "protocol") url->set_protocol(value);
    else if (field == "username") url->set_username(value);
    else if (field == "password") url->set_password(value);
    else if (field == "host") url->set_host(value);
    else if (field == "hostname") url->set_hostname(value);
    else if (field == "port") url->set_port(value);
    else if (field == "pathname") url->set_pathname(value);
    else if (field == "search") url->set_search(value);
    else if (field == "hash") url->set_hash(value);
    return url_record(*url);
}
} // namespace ais2api::waa
