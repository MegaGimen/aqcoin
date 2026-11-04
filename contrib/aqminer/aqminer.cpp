// Copyright (c) 2026 The AQCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// aqminer: a standalone CPU miner for AQCoin.
//
// It fetches block templates from a local node over JSON-RPC
// (getblocktemplate), builds its own coinbase (BIP34 height, extranonce and
// segwit witness commitment), assembles the header and searches nonces for a
// hash whose AQ value (hex digit sum) reaches the template's aq_threshold.
// Solutions are submitted with submitblock.

#include "aq_hash.h"

#include <pow_aq_table.h>

#include <univalue.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

// ---------------------------------------------------------------- utilities

std::string Timestamp()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
    return buf;
}

template <typename... Args>
void Log(const char* fmt, Args... args)
{
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    std::fprintf(stdout, "%s ", Timestamp().c_str());
    std::fprintf(stdout, fmt, args...);
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
}

Bytes ParseHex(const std::string& hex)
{
    if (hex.size() % 2) throw std::runtime_error("odd-length hex string");
    Bytes out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) out[i] = std::stoul(hex.substr(2 * i, 2), nullptr, 16);
    return out;
}

std::string HexStr(const unsigned char* p, size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = digits[p[i] >> 4];
        s[2 * i + 1] = digits[p[i] & 15];
    }
    return s;
}
std::string HexStr(const Bytes& b) { return HexStr(b.data(), b.size()); }

/** Parse a hash given in RPC (display, big-endian) order into internal byte order. */
Bytes ParseHashDisplay(const std::string& hex)
{
    Bytes b = ParseHex(hex);
    if (b.size() != 32) throw std::runtime_error("bad hash length");
    std::reverse(b.begin(), b.end());
    return b;
}

std::string Base64(const std::string& in)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(tbl[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(tbl[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

void PutLE32(Bytes& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xff);
}
void PutLE64(Bytes& b, uint64_t v)
{
    for (int i = 0; i < 8; ++i) b.push_back((v >> (8 * i)) & 0xff);
}
void PutCompactSize(Bytes& b, uint64_t n)
{
    if (n < 253) {
        b.push_back(n);
    } else if (n <= 0xffff) {
        b.push_back(253);
        b.push_back(n & 0xff);
        b.push_back(n >> 8);
    } else if (n <= 0xffffffff) {
        b.push_back(254);
        PutLE32(b, n);
    } else {
        b.push_back(255);
        PutLE64(b, n);
    }
}
void PutBytes(Bytes& b, const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }

/** Script push of an integer, identical to CScript() << int64 (as used by BIP34). */
void PushScriptInt(Bytes& script, int64_t n)
{
    if (n == -1 || (n >= 1 && n <= 16)) {
        script.push_back(n + 0x50); // OP_1NEGATE / OP_1..OP_16
        return;
    }
    if (n == 0) {
        script.push_back(0x00); // OP_0
        return;
    }
    Bytes num;
    const bool neg = n < 0;
    uint64_t absv = neg ? -static_cast<uint64_t>(n) : n;
    while (absv) {
        num.push_back(absv & 0xff);
        absv >>= 8;
    }
    if (num.back() & 0x80) {
        num.push_back(neg ? 0x80 : 0);
    } else if (neg) {
        num.back() |= 0x80;
    }
    script.push_back(num.size());
    PutBytes(script, num);
}

void PushScriptData(Bytes& script, const Bytes& data)
{
    if (data.size() >= 76) throw std::runtime_error("push too large");
    script.push_back(data.size());
    PutBytes(script, data);
}

Bytes DoubleSha(const Bytes& data)
{
    unsigned char h1[32], h2[32];
    CSHA256().Write(data.data(), data.size()).Finalize(h1);
    CSHA256().Write(h1, 32).Finalize(h2);
    return Bytes(h2, h2 + 32);
}

Bytes MerkleRoot(std::vector<Bytes> hashes)
{
    if (hashes.empty()) throw std::runtime_error("empty merkle tree");
    while (hashes.size() > 1) {
        if (hashes.size() % 2) hashes.push_back(hashes.back());
        std::vector<Bytes> next;
        for (size_t i = 0; i < hashes.size(); i += 2) {
            Bytes cat = hashes[i];
            PutBytes(cat, hashes[i + 1]);
            next.push_back(DoubleSha(cat));
        }
        hashes = std::move(next);
    }
    return hashes[0];
}

/** Expected number of hashes per block at threshold x (~ work(x)). */
double ExpectedHashes(uint32_t x)
{
    if (x > 960) return INFINITY;
    const uint256& w = AQ_WORK_TABLE[x];
    double d = 0;
    for (int i = 31; i >= 0; --i) d = d * 256.0 + w.data()[i];
    return d;
}

std::string FormatRate(double hps)
{
    char buf[64];
    if (hps >= 1e9) std::snprintf(buf, sizeof(buf), "%.2f GH/s", hps / 1e9);
    else if (hps >= 1e6) std::snprintf(buf, sizeof(buf), "%.2f MH/s", hps / 1e6);
    else if (hps >= 1e3) std::snprintf(buf, sizeof(buf), "%.2f kH/s", hps / 1e3);
    else std::snprintf(buf, sizeof(buf), "%.0f H/s", hps);
    return buf;
}

std::string FormatDuration(double secs)
{
    char buf[64];
    if (!std::isfinite(secs)) return "inf";
    if (secs < 120) std::snprintf(buf, sizeof(buf), "%.0fs", secs);
    else if (secs < 7200) std::snprintf(buf, sizeof(buf), "%.1fmin", secs / 60);
    else if (secs < 172800) std::snprintf(buf, sizeof(buf), "%.1fh", secs / 3600);
    else std::snprintf(buf, sizeof(buf), "%.1fd", secs / 86400);
    return buf;
}

// ---------------------------------------------------------------- JSON-RPC client

struct RpcConfig {
    std::string host{"127.0.0.1"};
    int port{9432};
    std::string user;
    std::string password;
    std::string cookie_file;
};

class RpcClient
{
public:
    explicit RpcClient(RpcConfig cfg) : m_cfg(std::move(cfg)) {}

    UniValue Call(const std::string& method, const UniValue& params, int timeout_sec = 30)
    {
        UniValue req(UniValue::VOBJ);
        req.pushKV("jsonrpc", "1.0");
        req.pushKV("id", "aqminer");
        req.pushKV("method", method);
        req.pushKV("params", params);
        const std::string body = req.write();

        const std::string auth = Base64(Credentials());
        std::string http = "POST / HTTP/1.1\r\nHost: " + m_cfg.host + "\r\nConnection: close\r\n"
                           "Authorization: Basic " + auth + "\r\nContent-Type: application/json\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

        const std::string response = Exchange(http, timeout_sec);
        const size_t hdr_end = response.find("\r\n\r\n");
        if (hdr_end == std::string::npos) throw std::runtime_error("malformed HTTP response");
        const std::string status_line = response.substr(0, response.find("\r\n"));
        if (status_line.find(" 401") != std::string::npos) throw std::runtime_error("RPC authentication failed (HTTP 401)");
        UniValue reply;
        if (!reply.read(response.substr(hdr_end + 4))) {
            throw std::runtime_error("cannot parse RPC reply: " + status_line);
        }
        const UniValue& err = reply.find_value("error");
        if (!err.isNull()) {
            throw std::runtime_error("RPC error in " + method + ": " + err.write());
        }
        return reply.find_value("result");
    }

private:
    std::string Credentials() const
    {
        if (!m_cfg.user.empty()) return m_cfg.user + ":" + m_cfg.password;
        std::ifstream f(m_cfg.cookie_file);
        std::string line;
        if (!f || !std::getline(f, line)) throw std::runtime_error("cannot read RPC cookie file " + m_cfg.cookie_file);
        return line;
    }

    std::string Exchange(const std::string& request, int timeout_sec)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(m_cfg.host.c_str(), std::to_string(m_cfg.port).c_str(), &hints, &res) != 0 || !res) {
            throw std::runtime_error("cannot resolve " + m_cfg.host);
        }
        int fd = -1;
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            timeval tv{timeout_sec, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        if (fd < 0) throw std::runtime_error("cannot connect to " + m_cfg.host + ":" + std::to_string(m_cfg.port));

        size_t sent = 0;
        while (sent < request.size()) {
            const ssize_t n = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                close(fd);
                throw std::runtime_error("send failed");
            }
            sent += n;
        }
        std::string out;
        char buf[65536];
        while (true) {
            const ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n < 0) {
                close(fd);
                throw std::runtime_error("receive failed or timed out");
            }
            if (n == 0) break;
            out.append(buf, n);
        }
        close(fd);
        return out;
    }

    RpcConfig m_cfg;
};

// ---------------------------------------------------------------- block template

struct TemplateTx {
    Bytes data; // full serialization
    Bytes txid; // internal byte order
};

struct BlockTemplate {
    int32_t version{0};
    Bytes prev_hash; // internal byte order
    uint32_t bits{0};
    int64_t height{0};
    int64_t mintime{0};
    int64_t curtime{0};
    int64_t coinbase_value{0};
    std::optional<Bytes> witness_commitment; // full scriptPubKey
    std::vector<TemplateTx> txs;
    std::string longpollid;
    uint64_t id{0}; // local generation counter
};

BlockTemplate ParseTemplate(const UniValue& t)
{
    BlockTemplate bt;
    bt.version = t.find_value("version").getInt<int32_t>();
    bt.prev_hash = ParseHashDisplay(t.find_value("previousblockhash").get_str());
    const UniValue& aq = t.find_value("aq_threshold");
    if (aq.isNull()) throw std::runtime_error("template has no aq_threshold: node is not an AQCoin node?");
    bt.bits = aq.getInt<uint32_t>();
    bt.height = t.find_value("height").getInt<int64_t>();
    bt.mintime = t.find_value("mintime").getInt<int64_t>();
    bt.curtime = t.find_value("curtime").getInt<int64_t>();
    bt.coinbase_value = t.find_value("coinbasevalue").getInt<int64_t>();
    const UniValue& wc = t.find_value("default_witness_commitment");
    if (!wc.isNull()) bt.witness_commitment = ParseHex(wc.get_str());
    for (const UniValue& tx : t.find_value("transactions").getValues()) {
        bt.txs.push_back({ParseHex(tx.find_value("data").get_str()), ParseHashDisplay(tx.find_value("txid").get_str())});
    }
    const UniValue& lp = t.find_value("longpollid");
    if (lp.isStr()) bt.longpollid = lp.get_str();
    return bt;
}

struct Coinbase {
    Bytes with_witness;
    Bytes txid;
};

Coinbase BuildCoinbase(const BlockTemplate& bt, const Bytes& payout_script, uint64_t extranonce)
{
    Bytes script_sig;
    PushScriptInt(script_sig, bt.height);
    Bytes en;
    PutLE64(en, extranonce);
    PushScriptData(script_sig, en);
    PushScriptData(script_sig, Bytes{'/', 'a', 'q', 'm', 'i', 'n', 'e', 'r', '/'});

    Bytes prefix; // version + inputs
    PutLE32(prefix, 2);
    Bytes body; // inputs .. outputs
    PutCompactSize(body, 1);
    body.insert(body.end(), 32, 0x00);
    PutLE32(body, 0xffffffff);
    PutCompactSize(body, script_sig.size());
    PutBytes(body, script_sig);
    PutLE32(body, 0xffffffff);
    const size_t n_out = bt.witness_commitment ? 2 : 1;
    PutCompactSize(body, n_out);
    PutLE64(body, bt.coinbase_value);
    PutCompactSize(body, payout_script.size());
    PutBytes(body, payout_script);
    if (bt.witness_commitment) {
        PutLE64(body, 0);
        PutCompactSize(body, bt.witness_commitment->size());
        PutBytes(body, *bt.witness_commitment);
    }
    Bytes locktime;
    PutLE32(locktime, 0);

    Coinbase cb;
    Bytes no_wit = prefix;
    PutBytes(no_wit, body);
    PutBytes(no_wit, locktime);
    cb.txid = DoubleSha(no_wit);
    if (bt.witness_commitment) {
        // Segwit serialization; the witness reserved value is 32 zero bytes,
        // which is what default_witness_commitment assumes.
        cb.with_witness = prefix;
        cb.with_witness.push_back(0x00); // marker
        cb.with_witness.push_back(0x01); // flag
        PutBytes(cb.with_witness, body);
        PutCompactSize(cb.with_witness, 1);
        PutCompactSize(cb.with_witness, 32);
        cb.with_witness.insert(cb.with_witness.end(), 32, 0x00);
        PutBytes(cb.with_witness, locktime);
    } else {
        cb.with_witness = no_wit;
    }
    return cb;
}

aqminer::Header80 BuildHeader(const BlockTemplate& bt, const Bytes& merkle_root, uint32_t time)
{
    aqminer::Header80 h{};
    aqminer::WriteLE32(h.data(), bt.version);
    std::memcpy(h.data() + 4, bt.prev_hash.data(), 32);
    std::memcpy(h.data() + 36, merkle_root.data(), 32);
    aqminer::WriteLE32(h.data() + 68, time);
    aqminer::WriteLE32(h.data() + 72, bt.bits);
    aqminer::WriteLE32(h.data() + 76, 0);
    return h;
}

// ---------------------------------------------------------------- miner

struct Options {
    RpcConfig rpc;
    std::string address;
    unsigned threads{1};
    bool benchmark{false};
    int benchmark_seconds{20};
    int poll_ms{1000};
    int stats_seconds{30};
    int template_refresh_seconds{30};
};

class Miner
{
public:
    Miner(const Options& opts, RpcClient& rpc, Bytes payout) : m_opts(opts), m_rpc(rpc), m_payout(std::move(payout)) {}

    void Run()
    {
        FetchTemplate();
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < m_opts.threads; ++t) workers.emplace_back([this, t] { Worker(t); });

        auto last_stats = std::chrono::steady_clock::now();
        auto last_template = last_stats;
        uint64_t last_hashes = 0;
        while (!m_quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_opts.poll_ms));
            const auto now = std::chrono::steady_clock::now();
            try {
                const std::string best = m_rpc.Call("getbestblockhash", UniValue(UniValue::VARR)).get_str();
                bool refresh = best != m_best_hash;
                if (!refresh && now - last_template > std::chrono::seconds(m_opts.template_refresh_seconds)) refresh = true;
                if (m_need_refresh.exchange(false)) refresh = true;
                if (refresh) {
                    if (best != m_best_hash) Log("new tip %s", best.c_str());
                    FetchTemplate();
                    last_template = now;
                }
            } catch (const std::exception& e) {
                Log("RPC error: %s (retrying)", e.what());
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
            if (now - last_stats >= std::chrono::seconds(m_opts.stats_seconds)) {
                const double secs = std::chrono::duration<double>(now - last_stats).count();
                const uint64_t total = m_hashes.load();
                const double rate = (total - last_hashes) / secs;
                last_hashes = total;
                last_stats = now;
                uint32_t bits;
                int64_t height;
                {
                    std::lock_guard<std::mutex> lock(m_mu);
                    bits = m_template.bits;
                    height = m_template.height;
                }
                Log("hashrate %s | height %lld | threshold %u | expected block time %s | found %llu accepted %llu",
                    FormatRate(rate).c_str(), (long long)height, bits,
                    FormatDuration(rate > 0 ? ExpectedHashes(bits) / rate : INFINITY).c_str(),
                    (unsigned long long)m_found.load(), (unsigned long long)m_accepted.load());
            }
        }
        for (auto& w : workers) w.join();
    }

private:
    void FetchTemplate()
    {
        UniValue rules(UniValue::VARR);
        rules.push_back("segwit");
        UniValue req(UniValue::VOBJ);
        req.pushKV("rules", rules);
        UniValue params(UniValue::VARR);
        params.push_back(req);
        BlockTemplate bt = ParseTemplate(m_rpc.Call("getblocktemplate", params));
        std::lock_guard<std::mutex> lock(m_mu);
        const bool changed = bt.prev_hash != m_template.prev_hash || bt.bits != m_template.bits;
        bt.id = ++m_template_counter;
        m_template = std::move(bt);
        Bytes disp = m_template.prev_hash;
        std::reverse(disp.begin(), disp.end());
        m_best_hash = HexStr(disp);
        m_generation.store(m_template.id);
        if (changed) {
            Log("template: height %lld, threshold %u (expected %.3g hashes), %zu txs",
                (long long)m_template.height, m_template.bits, ExpectedHashes(m_template.bits), m_template.txs.size());
        }
    }

    void Worker(unsigned thread_id)
    {
        std::mt19937_64 rng(std::random_device{}() ^ (uint64_t{thread_id} << 48));
        while (!m_quit) {
            BlockTemplate bt;
            {
                std::lock_guard<std::mutex> lock(m_mu);
                bt = m_template;
            }
            const uint64_t extranonce = rng();
            const Coinbase cb = BuildCoinbase(bt, m_payout, extranonce);
            std::vector<Bytes> hashes{cb.txid};
            for (const auto& tx : bt.txs) hashes.push_back(tx.txid);
            const Bytes merkle = MerkleRoot(hashes);
            const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const uint32_t time = static_cast<uint32_t>(std::max<int64_t>(bt.mintime, now));
            aqminer::Header80 header = BuildHeader(bt, merkle, time);

            // Stop flag: set when the template generation changes.
            std::atomic<bool> stop{false};
            std::atomic<bool> done{false};
            std::thread watcher([&] {
                while (!done) {
                    if (m_generation.load() != bt.id || m_quit) {
                        stop = true;
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
            // Search the nonce space in chunks so that the timestamp stays fresh.
            std::optional<uint32_t> nonce;
            for (uint64_t start = 0; start < (uint64_t{1} << 32) && !stop; start += (uint64_t{1} << 26)) {
                nonce = aqminer::Grind(header, bt.bits, start, start + (uint64_t{1} << 26), 1, stop, m_hashes);
                if (nonce) break;
            }
            done = true;
            watcher.join();
            if (!nonce) continue; // stale template or nonce space exhausted: new extranonce

            aqminer::WriteLE32(header.data() + 76, *nonce);
            unsigned char hash[32];
            {
                CSHA256 mid;
                mid.Write(header.data(), 64);
                aqminer::HashTail(mid, header.data() + 64, hash);
            }
            Submit(bt, header, cb, hash);
        }
    }

    void Submit(const BlockTemplate& bt, const aqminer::Header80& header, const Coinbase& cb, const unsigned char* hash)
    {
        ++m_found;
        Bytes block(header.begin(), header.end());
        PutCompactSize(block, 1 + bt.txs.size());
        PutBytes(block, cb.with_witness);
        for (const auto& tx : bt.txs) PutBytes(block, tx.data);
        Bytes disp(hash, hash + 32);
        std::reverse(disp.begin(), disp.end());
        Log("found block at height %lld: %s (AQ %u >= %u)", (long long)bt.height, HexStr(disp).c_str(), aqminer::AQ(hash), bt.bits);
        try {
            UniValue params(UniValue::VARR);
            params.push_back(HexStr(block));
            const UniValue res = m_rpc.Call("submitblock", params);
            if (res.isNull()) {
                ++m_accepted;
                Log("block accepted");
            } else {
                Log("block rejected: %s", res.write().c_str());
            }
        } catch (const std::exception& e) {
            Log("submitblock failed: %s", e.what());
        }
        m_need_refresh = true;
    }

    const Options& m_opts;
    RpcClient& m_rpc;
    const Bytes m_payout;
    std::mutex m_mu;
    BlockTemplate m_template;
    std::string m_best_hash;
    uint64_t m_template_counter{0};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<uint64_t> m_hashes{0};
    std::atomic<uint64_t> m_found{0};
    std::atomic<uint64_t> m_accepted{0};
    std::atomic<bool> m_need_refresh{false};
    std::atomic<bool> m_quit{false};
};

void RunBenchmark(const Options& opts)
{
    Log("benchmark: %u thread(s), %d seconds, SHA256 implementation: %s", opts.threads, opts.benchmark_seconds, SHA256AutoDetect().c_str());
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> hashes{0};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < opts.threads; ++t) {
        workers.emplace_back([&, t] {
            aqminer::Header80 header{};
            header[0] = t;
            header[1] = 0xa9;
            // Threshold 961 is unreachable, so this only measures hashing speed.
            aqminer::Grind(header, 961, 0, uint64_t{1} << 32, 1, stop, hashes);
        });
    }
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(opts.benchmark_seconds));
    stop = true;
    for (auto& w : workers) w.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double rate = hashes.load() / secs;
    Log("benchmark result: %s (%s per thread)", FormatRate(rate).c_str(), FormatRate(rate / opts.threads).c_str());
    for (uint32_t x : {560u, 600u, 650u, 700u, 750u, 800u}) {
        Log("  at threshold %u: expected time per block %s", x, FormatDuration(ExpectedHashes(x) / rate).c_str());
    }
}

void Usage()
{
    std::printf(
        "Usage: aqminer [options]\n"
        "\n"
        "Options:\n"
        "  -rpcconnect=<host>     RPC host (default: 127.0.0.1)\n"
        "  -rpcport=<port>        RPC port (default: 9432; testnet 19432; regtest 19543)\n"
        "  -testnet / -regtest    Use that network's default RPC port and cookie path\n"
        "  -datadir=<dir>         Node data directory, used to find the cookie (default: ~/.aqcoin)\n"
        "  -rpccookiefile=<file>  RPC cookie file (default: <datadir>[/testnet|/regtest]/.cookie)\n"
        "  -rpcuser=<user>        RPC user (instead of cookie authentication)\n"
        "  -rpcpassword=<pw>      RPC password\n"
        "  -address=<addr>        Payout address for mined blocks (required unless -benchmark)\n"
        "  -threads=<n>           Number of mining threads (default: 1)\n"
        "  -poll=<ms>             Tip polling interval in milliseconds (default: 1000)\n"
        "  -stats=<seconds>       Statistics interval (default: 30)\n"
        "  -benchmark[=<seconds>] Only measure the hash rate, without connecting to a node (default: 20 s)\n");
}

} // namespace

int main(int argc, char** argv)
{
    Options opts;
    std::string datadir;
    std::string net_subdir;
    bool port_set = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) arg = arg.substr(1);
        const size_t eq = arg.find('=');
        const std::string key = arg.substr(0, eq);
        const std::string val = eq == std::string::npos ? "" : arg.substr(eq + 1);
        if (key == "-h" || key == "-help" || key == "-?") {
            Usage();
            return 0;
        } else if (key == "-rpcconnect") {
            opts.rpc.host = val;
        } else if (key == "-rpcport") {
            opts.rpc.port = std::stoi(val);
            port_set = true;
        } else if (key == "-testnet") {
            net_subdir = "testnet";
            if (!port_set) opts.rpc.port = 19432;
        } else if (key == "-regtest") {
            net_subdir = "regtest";
            if (!port_set) opts.rpc.port = 19543;
        } else if (key == "-datadir") {
            datadir = val;
        } else if (key == "-rpccookiefile") {
            opts.rpc.cookie_file = val;
        } else if (key == "-rpcuser") {
            opts.rpc.user = val;
        } else if (key == "-rpcpassword") {
            opts.rpc.password = val;
        } else if (key == "-address") {
            opts.address = val;
        } else if (key == "-threads") {
            opts.threads = std::max(1, std::stoi(val));
        } else if (key == "-poll") {
            opts.poll_ms = std::max(50, std::stoi(val));
        } else if (key == "-stats") {
            opts.stats_seconds = std::max(1, std::stoi(val));
        } else if (key == "-benchmark") {
            opts.benchmark = true;
            if (!val.empty()) opts.benchmark_seconds = std::max(1, std::stoi(val));
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            Usage();
            return 1;
        }
    }

    SHA256AutoDetect();
    if (opts.benchmark) {
        RunBenchmark(opts);
        return 0;
    }
    if (opts.address.empty()) {
        std::fprintf(stderr, "Error: -address is required\n");
        return 1;
    }
    if (opts.rpc.user.empty() && opts.rpc.cookie_file.empty()) {
        if (datadir.empty()) {
            const char* home = std::getenv("HOME");
            datadir = std::string(home ? home : "") + "/.aqcoin";
        }
        opts.rpc.cookie_file = datadir + (net_subdir.empty() ? "" : "/" + net_subdir) + "/.cookie";
    }

    RpcClient rpc(opts.rpc);
    Bytes payout;
    try {
        UniValue params(UniValue::VARR);
        params.push_back(opts.address);
        const UniValue info = rpc.Call("validateaddress", params);
        if (!info.find_value("isvalid").get_bool()) {
            std::fprintf(stderr, "Error: invalid payout address for this network: %s\n", opts.address.c_str());
            return 1;
        }
        payout = ParseHex(info.find_value("scriptPubKey").get_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    Log("aqminer starting: %u thread(s), node %s:%d, SHA256 implementation: %s",
        opts.threads, opts.rpc.host.c_str(), opts.rpc.port, SHA256AutoDetect().c_str());
    while (true) {
        try {
            Miner miner(opts, rpc, payout);
            miner.Run();
            return 0;
        } catch (const std::exception& e) {
            Log("error: %s (restarting in 5 s)", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}
