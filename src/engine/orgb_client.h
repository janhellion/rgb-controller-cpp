#pragma once
// ── Minimal OpenRGB Client — single header, zero deps beyond POSIX ──
// Background reader thread consumes server responses.
// Supports UpdateZoneLEDs (1051) and UpdateLEDs (1050).
#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

namespace orgb_client {

#pragma pack(push, 1)
struct PacketHeader { char magic[4]; uint32_t device_id, packet_type, packet_size; };
#pragma pack(pop)

enum PacketType : uint32_t {
    REQUEST_PROTOCOL_VERSION   = 40,
    SET_CLIENT_NAME            = 50,
    DEVICE_LIST_UPDATED        = 100,
    RGBCONTROLLER_RESIZEZONE   = 1000,
    RGBCONTROLLER_UPDATELEDS   = 1050,
    RGBCONTROLLER_UPDATEZONELEDS = 1051,
};

struct Client {
    int sock = -1;
    std::atomic<bool> running{false};
    std::thread reader_thread;
    std::vector<uint8_t> recv_buf;

    bool connect(const char* host = "127.0.0.1", uint16_t port = 6742) {
        struct addrinfo hints{}, *res;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char ps[8]; snprintf(ps, sizeof(ps), "%u", port);
        if (getaddrinfo(host, ps, &hints, &res) != 0) return false;

        sock = socket(res->ai_family, res->ai_socktype, 0);
        if (sock < 0) { freeaddrinfo(res); return false; }

        struct timeval tv = {3, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
            freeaddrinfo(res); disconnect(); return false;
        }
        freeaddrinfo(res);

        uint32_t cp = 5; send_packet(0, REQUEST_PROTOCOL_VERSION, &cp, 4);
        PacketHeader hdr;
        if (recv_packet(hdr, recv_buf) && recv_buf.size() >= 4) {
            uint32_t sp = *reinterpret_cast<uint32_t*>(recv_buf.data());
            /* negotiated = min(client, server) */
        } else { /* fallback to v0 */ }

        tv = {1, 0}; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        send_packet(0, SET_CLIENT_NAME, "rgb-controller-cpp", 17);

        running = true;
        reader_thread = std::thread([this]() {
            PacketHeader h;
            while (running) {
                if (recv_packet(h, recv_buf)) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        return true;
    }

    void disconnect() {
        running = false;
        if (reader_thread.joinable()) reader_thread.join();
        if (sock >= 0) { ::close(sock); sock = -1; }
    }

    // Per-zone LED update (ASUS AURA addressable headers)
    bool update_zone_leds(uint32_t dev, uint32_t zone, const uint8_t* rgb, uint32_t n) {
        uint32_t sz = 4 + 4 + 2 + n * 4;
        uint8_t buf[1024];
        *reinterpret_cast<uint32_t*>(buf)     = sz;
        *reinterpret_cast<uint32_t*>(buf + 4) = zone;
        *reinterpret_cast<uint16_t*>(buf + 8) = (uint16_t)n;
        pack_colors(buf + 10, rgb, n);
        return send_packet(dev, RGBCONTROLLER_UPDATEZONELEDS, buf, sz);
    }

    // Device-level LED update (Logitech mice, keyboards)
    bool update_leds(uint32_t dev, const uint8_t* rgb, uint32_t n) {
        uint32_t sz = 4 + 2 + n * 4;
        uint8_t buf[1024];
        *reinterpret_cast<uint32_t*>(buf)     = sz;
        *reinterpret_cast<uint16_t*>(buf + 4) = (uint16_t)n;
        pack_colors(buf + 6, rgb, n);
        return send_packet(dev, RGBCONTROLLER_UPDATELEDS, buf, sz);
    }

    bool resize_zone(uint32_t dev, uint32_t zone, uint32_t size) {
        uint32_t d[2] = {zone, size};
        return send_packet(dev, RGBCONTROLLER_RESIZEZONE, d, 8);
    }

private:
    static void pack_colors(uint8_t* dst, const uint8_t* rgb, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t off = i * 4;
            dst[off]=rgb[i*3]; dst[off+1]=rgb[i*3+1]; dst[off+2]=rgb[i*3+2]; dst[off+3]=0;
        }
    }

    bool write_all(const void* b, size_t n) {
        size_t t = 0;
        while (t < n) { ssize_t r = ::write(sock, (const char*)b + t, n - t); if (r <= 0) return false; t += r; }
        return true;
    }

    bool read_all(void* b, size_t n) {
        size_t t = 0;
        while (t < n) { ssize_t r = ::read(sock, (char*)b + t, n - t); if (r <= 0) return false; t += r; }
        return true;
    }

    bool send_packet(uint32_t dev, uint32_t type, const void* data, uint32_t sz) {
        PacketHeader hdr{};
        memcpy(hdr.magic, "ORGB", 4);
        hdr.device_id = dev; hdr.packet_type = type; hdr.packet_size = sz;
        return write_all(&hdr, sizeof(hdr)) && (sz == 0 || write_all(data, sz));
    }

    bool recv_packet(PacketHeader& hdr, std::vector<uint8_t>& data) {
        if (!read_all(&hdr, sizeof(hdr))) return false;
        if (hdr.packet_size == 0) return true;
        data.resize(hdr.packet_size);
        return read_all(data.data(), hdr.packet_size);
    }
};

} // namespace orgb_client
