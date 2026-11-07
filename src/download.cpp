// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include "ais2api/download.hpp"
#include "ais2api/console.hpp"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <charconv>
#include <cctype>
#include <optional>
namespace ais2api {
namespace {
struct CurlInit {
    CurlInit() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT))
            throw Error(500, "network_init", "Cannot initialize downloader");
    }
    ~CurlInit() { curl_global_cleanup(); }
};
using Curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
Curl curl_handle(const download_detail::Options &options) {
    static CurlInit init;
    Curl c(curl_easy_init(), curl_easy_cleanup);
    if (!c)
        throw Error(500, "network_init", "Cannot allocate downloader");
    curl_easy_setopt(c.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c.get(), CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c.get(), CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c.get(), CURLOPT_USERAGENT, "ais2api/0.1");
    if (!options.ca_file.empty()) {
        auto ca = options.ca_file.string();
        curl_easy_setopt(c.get(), CURLOPT_CAINFO, ca.c_str());
    }
    return c;
}
std::string file_digest(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error(500, "download_read", "Cannot read downloaded archive");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw Error(500, "digest", "Cannot initialize digest");
    std::array<char, 65536> bytes{};
    while (in) {
        in.read(bytes.data(), bytes.size());
        auto n = in.gcount();
        if (n > 0 &&
            EVP_DigestUpdate(context.get(), bytes.data(), static_cast<std::size_t>(n)) != 1)
            throw Error(500, "digest", "Cannot update digest");
    }
    if (!in.eof())
        throw Error(500, "download_read", "Cannot read complete archive");
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned n{};
    if (EVP_DigestFinal_ex(context.get(), digest, &n) != 1)
        throw Error(500, "digest", "Cannot finalize digest");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < n; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 15];
    }
    return out;
}
}

namespace {
// An attempt that a later one resumes.
void interrupted(std::string reason) {
    console::emit_record({{"event", "setup_download_interrupted"}, {"reason", std::move(reason)}});
}
std::uint64_t range_bytes(const fs::path &part) {
    std::uint64_t bytes = 0;
    std::error_code error;
    for (unsigned i = 0; i < 4; ++i) {
        auto segment = part; segment += ".range" + std::to_string(i);
        if (const auto size = fs::file_size(segment, error); !error) bytes += size;
    }
    return bytes;
}
// After the given number of consecutive failures without progress, which a
// download that made progress starts again from one.
std::chrono::milliseconds backoff(const download_detail::Options &options, unsigned failures) {
    return options.retry_delay * (1u << (failures - 1));
}
void cleanup_ranges(const fs::path &part) {
    std::error_code error;
    for (unsigned i = 0; i < 4; ++i) {
        auto segment = part; segment += ".range" + std::to_string(i);
        fs::remove(segment, error);
    }
    auto marker = part; marker += ".ranges.json";
    fs::remove(marker, error);
}
struct RangeTransfer {
    std::ofstream output;
    std::string range, etag, response_range, response_etag;
    std::uint64_t received = 0, limit = 0, first = 0, last = 0, total = 0;
    long status = 0;
    std::uint64_t existing = 0;
    bool valid = false;
};
size_t range_header(char *data, size_t size, size_t count, void *opaque) noexcept {
    try {
    auto &t = *static_cast<RangeTransfer *>(opaque);
    std::string_view line(data, size*count);
    if (line.starts_with("HTTP/")) {
        t.response_range.clear(); t.response_etag.clear(); t.valid = false;
        auto space = line.find(' ');
        if (space != line.npos) std::from_chars(line.data()+space+1, line.data()+line.size(), t.status);
    } else {
        auto colon = line.find(':');
        if (colon != line.npos) {
            std::string name(line.substr(0, colon));
            for (auto &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto value = line.substr(colon+1);
            while (!value.empty() && (value.front()==' ' || value.front()=='\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back()=='\r' || value.back()=='\n' || value.back()==' ')) value.remove_suffix(1);
            if (name == "content-range") t.response_range = value;
            if (name == "etag") t.response_etag = value;
        }
    }
    return size*count;
    } catch (...) { return 0; }
}
// "unsupported": the server does not serve exact ranges, so a single stream
// takes over. "interrupted": a transfer failed (a TLS handshake through a
// proxy, a dropped connection); another attempt resumes each range.
enum class Segments { complete, unsupported, interrupted };
// Each response is stored separately: an interrupted segment is resumable without
// treating sparse/preallocated bytes as downloaded data. Four easy handles share
// one bounded multi loop, so failure cleanup never leaves worker threads running.
Segments segmented(std::string_view url, const fs::path &part, std::string_view digest,
                   std::string_view proxy, std::uint64_t total, const download_detail::Options &options) {
    if (options.connections == 1 || total < 4*1024*1024 || (fs::exists(part) && fs::file_size(part))) return Segments::unsupported;
    std::string address(url), proxy_text(proxy);
    auto configure = [&](CURL *c, RangeTransfer &t) {
        curl_easy_setopt(c, CURLOPT_URL, address.c_str());
        if (!proxy_text.empty())
            curl_easy_setopt(c, CURLOPT_PROXY, proxy_text.c_str());
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 1800L);
        curl_easy_setopt(c, CURLOPT_RANGE, t.range.c_str());
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, range_header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
    };
    RangeTransfer probe;
    probe.range = "0-0";
    auto handle = curl_handle(options); configure(handle.get(), probe);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION,
        +[](char *,size_t s,size_t n,void *p)->size_t {
            auto &t=*static_cast<RangeTransfer *>(p);
            if (s*n > 1-t.received) return 0;
            t.received += s*n; return s*n;
        });
    // A write error is the probe refusing more than the one byte it asked for.
    const auto probed = curl_easy_perform(handle.get());
    // An untrusted certificate is not a transient failure: the single stream
    // reports it.
    if (probed == CURLE_PEER_FAILED_VERIFICATION) return Segments::unsupported;
    if (probed != CURLE_OK && probed != CURLE_WRITE_ERROR) {
        interrupted(curl_easy_strerror(probed));
        return Segments::interrupted;
    }
    if (probed != CURLE_OK || probe.status != 206 || probe.received != 1 ||
        !download_detail::content_range(probe.response_range,0,0,total) ||
        probe.response_etag.size()<2 || probe.response_etag.front()!='"' || probe.response_etag.back()!='"') return Segments::unsupported;
    auto boundary=[&](unsigned i){return (total/4)*i+(total%4)*i/4;};
    std::array<RangeTransfer,4> transfers;
    std::array<Curl,4> handles{curl_handle(options),curl_handle(options),curl_handle(options),curl_handle(options)};
    std::array<fs::path,4> paths;
    auto marker=part; marker += ".ranges.json";
    const auto identity=json::serialize(json::object{{"sha256",std::string(digest)}, {"etag",probe.response_etag}, {"size",total}, {"url",address}});
    bool resume=fs::is_regular_file(marker) && !fs::is_symlink(fs::symlink_status(marker)) && read_file(marker,65536)==identity;
    if (fs::is_symlink(fs::symlink_status(marker))) throw Error(400,"unsafe_path","Download marker cannot be a symbolic link");
    atomic_write(marker,identity,true);
    std::unique_ptr<CURLM,decltype(&curl_multi_cleanup)> multi(curl_multi_init(),curl_multi_cleanup);
    if (!multi) throw Error(500,"network_init","Cannot allocate parallel downloader");
    std::array<curl_slist *,4> headers{};
    struct Cleanup {
        CURLM *multi; std::array<Curl,4> &handles; std::array<curl_slist *,4> &headers;
        ~Cleanup(){ for(unsigned i=0;i<4;++i){curl_multi_remove_handle(multi,handles[i].get());curl_slist_free_all(headers[i]);} }
    } cleanup{multi.get(),handles,headers};
    for (unsigned i=0;i<4;++i) {
        auto &t=transfers[i]; paths[i]=part; paths[i]+=".range"+std::to_string(i);
        if (fs::is_symlink(fs::symlink_status(paths[i]))) throw Error(400,"unsafe_path","Download segment cannot be a symbolic link");
        if (fs::exists(paths[i]) && !fs::is_regular_file(paths[i]))
            throw Error(400,"unsafe_path","Download segment must be a regular file");
        auto first=boundary(i), last=boundary(i+1)-1;
        auto old=resume && fs::is_regular_file(paths[i]) ? fs::file_size(paths[i]) : 0;
        if (old>last-first+1) old=0;
        if (!old) {std::ofstream truncate(paths[i],std::ios::binary|std::ios::trunc);}
        t.existing=old; t.limit=last-first+1-old; t.etag=probe.response_etag;
        if (!t.limit) {t.valid=true;continue;}
        t.first=first+old; t.last=last; t.total=total;
        t.range=std::to_string(first+old)+"-"+std::to_string(last);
        t.output.open(paths[i],std::ios::binary|std::ios::app);
        if (!t.output) throw Error(500,"download_write","Cannot write archive segment");
        configure(handles[i].get(),t);
        headers[i]=curl_slist_append(nullptr,("If-Range: "+t.etag).c_str());
        curl_easy_setopt(handles[i].get(),CURLOPT_HTTPHEADER,headers[i]);
        curl_easy_setopt(handles[i].get(),CURLOPT_WRITEFUNCTION,
          +[](char *b,size_t s,size_t n,void *p)->size_t {
            auto &t=*static_cast<RangeTransfer *>(p); auto count=s*n;
            // Validate before writing, including servers that ignore Range/If-Range.
            if(t.status!=206 || t.response_etag!=t.etag || !download_detail::content_range(t.response_range,t.first,t.last,t.total) || count>t.limit-t.received) return 0;
            t.output.write(b,static_cast<std::streamsize>(count));
            if(!t.output)return 0; t.received+=count;return count;
          });
        curl_multi_add_handle(multi.get(),handles[i].get());
    }
    int running=0; auto logged=Clock::now(); bool transport_ok=true;
    // Each transfer's result once curl is done with it. A write error is a
    // response the write callback refused.
    std::array<std::optional<CURLcode>,4> results;
    do {
        if(curl_multi_perform(multi.get(),&running)!=CURLM_OK){transport_ok=false;break;}
        int remaining=0;
        while(auto *message=curl_multi_info_read(multi.get(),&remaining))
            if(message->msg==CURLMSG_DONE) {
                for(unsigned i=0;i<4;++i) if(handles[i].get()==message->easy_handle) results[i]=message->data.result;
                if(message->data.result!=CURLE_OK) transport_ok=false;
            }
        if(Clock::now()-logged>=std::chrono::seconds(5)){
            std::uint64_t bytes=0; for(auto &t:transfers) bytes+=t.existing+t.received;
            console::emit_record({{"event","setup_download"},{"received_bytes",bytes},{"maximum_bytes",total},{"connections",4}});logged=Clock::now();
        }
        if(running && transport_ok) {int ready=0; if(curl_multi_poll(multi.get(),nullptr,0,1000,&ready)!=CURLM_OK) transport_ok=false;}
    }while(running && transport_ok);
    for(auto &c:handles) curl_multi_remove_handle(multi.get(),c.get());
    bool valid=transport_ok;
    for(unsigned i=0;i<4;++i){auto &t=transfers[i];t.output.close();
        auto last=boundary(i+1)-1;
        valid=valid && (t.valid || (t.received==t.limit && t.status==206 && t.response_etag==t.etag && download_detail::content_range(t.response_range,last-t.limit+1,last,total)));
    }
    if(!valid) {
        // A range answered with another 2xx response, or another entity, means
        // the server does not serve exact ranges: that range's bytes are dropped
        // and a single stream takes over. After any other failure (transport,
        // an HTTP error) each range keeps the bytes the write callback checked,
        // and the next attempt resumes them.
        bool refused=false; std::string reason;
        for(unsigned i=0;i<4;++i){auto &t=transfers[i];auto last=boundary(i+1)-1;
            if(t.valid || !results[i]) continue;
            if(*results[i]!=CURLE_OK && *results[i]!=CURLE_WRITE_ERROR) {
                if(reason.empty()) reason=curl_easy_strerror(*results[i]);
                continue;
            }
            const bool exact=t.status==206 && t.response_etag==t.etag && download_detail::content_range(t.response_range,last-t.limit+1,last,total);
            if(!exact && (t.status==200 || t.status==206)) {refused=true; fs::remove(paths[i]);}
            else if(!exact && reason.empty()) reason="HTTP "+std::to_string(t.status);
        }
        if(refused) return Segments::unsupported;
        interrupted(reason.empty() ? "transfer stopped" : reason);
        return Segments::interrupted;
    }
    std::ofstream out(part,std::ios::binary|std::ios::trunc);
    std::array<char,65536> buffer{};
    for(auto &path:paths){std::ifstream in(path,std::ios::binary);while(in){in.read(buffer.data(),buffer.size());out.write(buffer.data(),in.gcount());}if(!in.eof())throw Error(500,"download_read","Cannot read archive segment");}
    out.close();
    if(!out || fs::file_size(part)!=total) throw Error(500,"download_write","Cannot assemble archive segments");
    for (auto &path : paths)
        fs::remove(path);
    fs::remove(marker);
    return Segments::complete;
}
}
void download_detail::download_with_options(std::string_view url, const fs::path &destination, std::string_view digest,
                       std::string_view proxy, std::uint64_t expected_size, const Options &options) {
    if (options.connections != 1 && options.connections != 4)
        throw Error(400,"invalid_download_options","Downloader supports one or four connections");
    if (!url.starts_with("https://") || digest.size() != 64 ||
        digest.find_first_not_of("0123456789abcdef") != std::string_view::npos)
        throw Error(400, "invalid_artifact", "Artifact requires HTTPS and a SHA-256 digest");
    fs::create_directories(destination.parent_path());
    auto part = destination;
    part += ".part";
    if (fs::is_symlink(fs::symlink_status(part)) || fs::is_symlink(fs::symlink_status(destination)))
        throw Error(400, "unsafe_path", "Download path cannot be a symbolic link");
    if (fs::is_regular_file(destination) && (!expected_size || fs::file_size(destination) == expected_size) && file_digest(destination) == digest) {
        cleanup_ranges(part);
        return;
    }
    for (unsigned failures = 0; failures < 5;) {
        const auto before = range_bytes(part);
        const auto outcome = segmented(url, part, digest, proxy, expected_size, options);
        if (outcome == Segments::unsupported) break;
        if (outcome == Segments::interrupted) {
            failures = range_bytes(part) > before ? 1 : failures + 1;
            std::this_thread::sleep_for(backoff(options, failures));
            continue;
        }
        if (file_digest(part) != digest) {
            fs::remove(part);
            throw Error(502,"digest_mismatch","Browser archive SHA-256 mismatch; partial archive removed");
        }
        if (fs::exists(destination)) fs::remove(destination);
        fs::rename(part,destination);
        cleanup_ranges(part);
        return;
    }
    for (unsigned failures = 0; failures < 5;) {
        auto offset = fs::exists(part) ? fs::file_size(part) : 0;
        const auto cap = expected_size ? expected_size : 1024ull * 1024 * 1024;
        if (offset > cap) {
            fs::remove(part);
            throw Error(413, "download_limit",
                        "Partial download exceeded its permitted archive size");
        }
        if (offset && (!expected_size || offset == expected_size) && file_digest(part) == digest) {
            if (fs::exists(destination))
                fs::remove(destination);
            fs::rename(part, destination);
            cleanup_ranges(part);
            return;
        }
        std::ofstream out(part, std::ios::binary | std::ios::app);
        if (!out)
            throw Error(500, "download_write", "Cannot write download cache");
        auto c = curl_handle(options);
        std::string address(url), proxy_value(proxy);
        curl_easy_setopt(c.get(), CURLOPT_URL, address.c_str());
        curl_easy_setopt(c.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c.get(), CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c.get(), CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(c.get(), CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(offset));
        if (!proxy_value.empty())
            curl_easy_setopt(c.get(), CURLOPT_PROXY, proxy_value.c_str());
        struct Download {
            std::ofstream *output;
            std::uint64_t bytes, cap;
            bool exceeded = false;
            Clock::time_point logged = Clock::now();
        } progress{&out, offset, cap};
        curl_easy_setopt(c.get(), CURLOPT_TIMEOUT, 1800L);
        curl_easy_setopt(c.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c.get(), CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(
            c.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept -> int {
                try {
                auto &state = *static_cast<Download *>(p);
                if (Clock::now() - state.logged >= std::chrono::seconds(5)) {
                    console::emit_record({{"event", "setup_download"},
                                          {"received_bytes", state.bytes},
                                          {"maximum_bytes", state.cap}});
                    state.logged = Clock::now();
                }
                return 0;
                } catch (...) { return 1; }
            });
        curl_easy_setopt(c.get(), CURLOPT_WRITEDATA, &progress);
        curl_easy_setopt(
            c.get(), CURLOPT_WRITEFUNCTION, +[](char *b, size_t s, size_t n, void *p) -> size_t {
                auto size = s * n;
                auto &state = *static_cast<Download *>(p);
                if (size > state.cap - state.bytes) {
                    state.exceeded = true;
                    return 0;
                }
                state.output->write(b, static_cast<std::streamsize>(size));
                if (!*state.output)
                    return 0;
                state.bytes += size;
                return size;
            });
        auto result = curl_easy_perform(c.get());
        long status = 0;
        curl_easy_getinfo(c.get(), CURLINFO_RESPONSE_CODE, &status);
        out.close();
        if (progress.exceeded) {
            fs::remove(part);
            throw Error(413, "download_limit", "Browser archive exceeded its locked size");
        }
        if (result == CURLE_OK) {
            if ((expected_size && fs::file_size(part) != expected_size) || file_digest(part) != digest) {
                fs::remove(part);
                throw Error(502, "digest_mismatch",
                            "Browser archive SHA-256 mismatch; partial archive removed");
            }
            if (fs::exists(destination))
                fs::remove(destination);
            fs::rename(part, destination);
            cleanup_ranges(part);
            return;
        }
        if (result == CURLE_RANGE_ERROR || status == 416) {
            fs::remove(part);
            ++failures;
            continue;
        }
        std::error_code error;
        const auto received = fs::file_size(part, error);
        failures = !error && received > offset ? 1 : failures + 1;
        if (failures == 5 || result == CURLE_PEER_FAILED_VERIFICATION)
            throw Error(502, "download_failed",
                        std::string("Archive download failed: ") + curl_easy_strerror(result) +
                            "; partial bytes retained for a later retry");
        interrupted(result == CURLE_HTTP_RETURNED_ERROR ? "HTTP " + std::to_string(status) : curl_easy_strerror(result));
        std::this_thread::sleep_for(backoff(options, failures));
    }
    throw Error(502, "download_failed", "Archive server did not accept resumed downloads");
}
void download_verified(std::string_view url, const fs::path &destination, std::string_view digest,
                       std::string_view proxy, std::uint64_t expected_size) {
    download_detail::download_with_options(url, destination, digest, proxy, expected_size, {});
}

}
