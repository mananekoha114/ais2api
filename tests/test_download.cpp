// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/download.hpp"
#include "ais2api/runtime.hpp"
#include "tls_fixture.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
using namespace ais2api;
namespace {
struct DownloadFixture {
    fs::path root = fs::temp_directory_path() / ("ais2api-https-" + random_hex(8));
    boost::asio::io_context io;
    boost::asio::ssl::context tls{boost::asio::ssl::context::tls_server};
    boost::asio::ip::tcp::acceptor acceptor{io,{boost::asio::ip::make_address("127.0.0.1"),0}};
    std::jthread accept_thread;
    std::vector<std::jthread> clients;
    // 0 correct; 1 ignores ranges; 2 changes ETag; 3 malformed ranges;
    // 4 interrupts range transfers and rejects stream fallback; 5 answers the
    // first range transfer with an HTTP error.
    std::atomic<int> mode{0}, active{0}, peak{0}, resumed{0}, full_requests{0}, errors{0};
    std::chrono::milliseconds throttle{0};
    const std::string body=std::string(4*1024*1024,'x');
    DownloadFixture() {
        fs::create_directories(root);
        const auto [certpem,keypem]=test::self_signed_localhost();
        atomic_write(root/"ca.pem",certpem);
        tls.use_certificate_chain(boost::asio::buffer(certpem));
        tls.use_private_key(boost::asio::buffer(keypem),boost::asio::ssl::context::pem);
        acceptor.non_blocking(true);
        accept_thread=std::jthread([&](std::stop_token stop){
            while(!stop.stop_requested()) {
                boost::asio::ip::tcp::socket socket(io); boost::system::error_code error;
                acceptor.accept(socket,error);
                if(error){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
                clients.emplace_back([this,socket=std::move(socket)]() mutable { serve(std::move(socket)); });
            }
        });
    }
    ~DownloadFixture(){accept_thread.request_stop();accept_thread.join();clients.clear();std::error_code ec;fs::remove_all(root,ec);}
    std::string url() const {return "https://127.0.0.1:"+std::to_string(acceptor.local_endpoint().port())+"/archive";}
    download_detail::Options options(unsigned connections=4) const {return {connections,root/"ca.pem",std::chrono::milliseconds(10)};}
    std::string digest() const {
        unsigned char hash[EVP_MAX_MD_SIZE];unsigned size=0;
        EVP_Digest(body.data(),body.size(),hash,&size,EVP_sha256(),nullptr);
        std::string result;const char hex[]="0123456789abcdef";
        for(unsigned i=0;i<size;++i){result+=hex[hash[i]>>4];result+=hex[hash[i]&15];}return result;
    }
    void serve(boost::asio::ip::tcp::socket socket) {
        try {
            boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(std::move(socket),tls);
            stream.handshake(boost::asio::ssl::stream_base::server);
            boost::asio::streambuf request;boost::asio::read_until(stream,request,"\r\n\r\n");
            std::string text(boost::asio::buffers_begin(request.data()),boost::asio::buffers_end(request.data()));
            auto start=text.find("Range: bytes="); bool range=start!=std::string::npos;
            std::size_t first=0,last=body.size()-1;
            if(range){start+=13;auto dash=text.find('-',start);first=std::stoull(text.substr(start,dash-start));auto end=text.find("\r\n",dash);if(end>dash+1)last=std::stoull(text.substr(dash+1,end-dash-1));}
            bool probe=range && first==0 && last==0;
            const int behavior=mode.load();
            if(!range)++full_requests;
            if(range && !probe && first%(body.size()/4)!=0)++resumed;
            if(behavior==5 && range && !probe && errors++==0){boost::asio::write(stream,boost::asio::buffer(std::string("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 4\r\n\r\nbusy")));return;}
            if(behavior==1)range=false;
            if(first>=body.size() || last>=body.size() || first>last) return;
            if(!range){first=0;last=body.size()-1;}
            std::string header="HTTP/1.1 "+std::string(!range && behavior==4?"503 Unavailable":range?"206 Partial Content":"200 OK")+"\r\nConnection: close\r\n";
            if(!range && behavior==4){header+="Content-Length: 0\r\n\r\n";boost::asio::write(stream,boost::asio::buffer(header));return;}
            auto tag=behavior==2 && !probe?"changed":"fixture";
            header+="ETag: \""+std::string(tag)+"\"\r\nContent-Length: "+std::to_string(last-first+1)+"\r\n";
            if(range) header+="Content-Range: bytes "+std::to_string(first+(behavior==3 && !probe?1:0))+"-"+std::to_string(last)+"/"+std::to_string(body.size())+"\r\n";
            header+="\r\n";boost::asio::write(stream,boost::asio::buffer(header));
            int current=++active,maximum=peak.load();while(current>maximum && !peak.compare_exchange_weak(maximum,current)){}
            struct Release{std::atomic<int>&active;~Release(){--active;}} release{active};
            auto end=last+1;if(behavior==4 && !probe)end=first+(last-first+1)/2;
            for(auto cursor=first;cursor<end;){auto bytes=std::min<std::size_t>(16384,end-cursor);boost::asio::write(stream,boost::asio::buffer(body.data()+cursor,bytes));cursor+=bytes;if(throttle.count() && !probe)std::this_thread::sleep_for(throttle);}
        } catch(const std::exception &) {} // Client intentionally aborts invalid/oversized responses.
    }
};
}

TEST_CASE("download ranges require exact bounded response identity", "[setup][download]") {
    using download_detail::content_range;
    REQUIRE(content_range("bytes 0-0/100",0,0,100));
    REQUIRE(content_range("bytes 50-99/100",50,99,100));
    for(auto value : {"bytes 0-99/100", "bytes 50-99/*", "bytes 50-99/101", "bytes 50-100/100",
                       "bytes 50-99/100 trailing", "bytes -50-99/100", "bytes 18446744073709551616-99/100"})
        REQUIRE_FALSE(content_range(value,50,99,100));
    REQUIRE_FALSE(content_range("bytes 0-0/0",0,0,0));
}
TEST_CASE("download cached archives must match expected size and digest", "[setup][download]") {
    auto root=fs::temp_directory_path() / ("ais2api-download-"+random_hex(8));
    fs::create_directories(root);
    auto destination=root/"browser.zip";
    atomic_write(destination,"abc");
    const std::string digest="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    REQUIRE_THROWS_AS(download_verified("https://example.invalid/archive",destination,std::string(64,'z'),"",3), Error);
    REQUIRE_NOTHROW(download_verified("https://example.invalid/archive",destination,digest,"",3));
    fs::rename(destination,root/"browser.zip.part");
    REQUIRE_NOTHROW(download_verified("https://example.invalid/archive",destination,digest,"",3));
    REQUIRE(read_file(destination)=="abc");
    fs::remove_all(root);
}

TEST_CASE("HTTPS segmented download validates every range and falls back safely", "[setup][download]") {
    DownloadFixture server;
    for(int mode : {0,1,2,3}) {
        server.mode=mode;
        auto target=server.root/("archive-"+std::to_string(mode));
        CAPTURE(mode);
        REQUIRE_NOTHROW(download_detail::download_with_options(server.url(),target,server.digest(),"",server.body.size(),server.options()));
        REQUIRE(read_file(target,server.body.size()+1)==server.body);
    }
    REQUIRE(server.full_requests>=3);
    for(int mode : {1,2,3}) {
        auto part=server.root/("archive-"+std::to_string(mode)+".part");
        REQUIRE_FALSE(fs::exists(fs::path(part.string()+".ranges.json")));
        REQUIRE_FALSE(fs::exists(fs::path(part.string()+".range0")));
    }
}
TEST_CASE("HTTPS interrupted segments resume only verified response prefixes", "[setup][download]") {
    DownloadFixture server;
    server.mode=4;
    auto target=server.root/"archive";
    REQUIRE_THROWS_AS(download_detail::download_with_options(server.url(),target,server.digest(),"",server.body.size(),server.options()),Error);
    REQUIRE_FALSE(fs::exists(target));
    server.mode=0;
    REQUIRE_NOTHROW(download_detail::download_with_options(server.url(),target,server.digest(),"",server.body.size(),server.options()));
    REQUIRE(server.resumed>0);
    REQUIRE(read_file(target,server.body.size()+1)==server.body);
}
TEST_CASE("HTTPS segmented download resumes its ranges after an HTTP error", "[setup][download]") {
    DownloadFixture server;
    server.mode=5;
    auto target=server.root/"archive";
    REQUIRE_NOTHROW(download_detail::download_with_options(server.url(),target,server.digest(),"",server.body.size(),server.options()));
    REQUIRE(read_file(target,server.body.size()+1)==server.body);
    REQUIRE(server.errors>=2);
    REQUIRE(server.full_requests==0);
}
TEST_CASE("HTTPS single stream resumes partial files and restarts ignored ranges", "[setup][download]") {
    DownloadFixture server;
    for (int mode : {0,1}) {
        server.mode=mode;
        auto target=server.root/("single-"+std::to_string(mode));
        atomic_write(fs::path(target.string()+".part"),server.body.substr(0,12345));
        REQUIRE_NOTHROW(download_detail::download_with_options(server.url(),target,server.digest(),"",server.body.size(),server.options(1)));
        REQUIRE(read_file(target,server.body.size()+1)==server.body);
    }
    REQUIRE(server.resumed>=2);
}
TEST_CASE("HTTPS downloader enforces size hash and certificate trust", "[setup][download]") {
    DownloadFixture server;
    auto target=server.root/"archive";
    REQUIRE_THROWS_AS(download_detail::download_with_options(server.url(),target,server.digest(),"",1024,server.options(1)),Error);
    REQUIRE_FALSE(fs::exists(target));
    REQUIRE_FALSE(fs::exists(server.root/"archive.part"));
    REQUIRE_THROWS_AS(download_detail::download_with_options(server.url(),target,std::string(64,'0'),"",server.body.size(),server.options()),Error);
    REQUIRE_FALSE(fs::exists(target));
    REQUIRE_THROWS_AS(download_detail::download_with_options(server.url(),target,server.digest(),"",server.body.size(),{1,{}}),Error);
    REQUIRE_FALSE(fs::exists(target));
}
TEST_CASE("four HTTPS connections improve per-connection throttled throughput", "[download][benchmark]") {
    DownloadFixture server;
    server.throttle=std::chrono::milliseconds(3);
    auto run=[&](unsigned connections){
        auto begin=Clock::now();
        download_detail::download_with_options(server.url(),server.root/std::to_string(connections),server.digest(),"",server.body.size(),server.options(connections));
        return std::chrono::duration<double>(Clock::now()-begin).count();
    };
    auto single=run(1),parallel=run(4);
    std::cerr<<"download benchmark: bytes="<<server.body.size()<<" single_seconds="<<single<<" four_seconds="<<parallel<<" speedup="<<single/parallel<<" peak_connections="<<server.peak<<'\n';
    REQUIRE(server.peak>=2);
    // Synthetic per-connection bandwidth limit; this does not predict Internet/CDN speed.
    REQUIRE(parallel<single*0.85);
}
