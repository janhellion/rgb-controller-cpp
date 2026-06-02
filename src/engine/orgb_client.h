#pragma once
// ─────────────────────────────────────────────────────────
//  Minimal OpenRGB Client — single header, zero deps
//  Fire-and-forget color updates with background reader
//  thread to consume server responses (no drain hacks).
// ─────────────────────────────────────────────────────────
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

namespace orgb_client {

#pragma pack(push, 1)
struct PacketHeader {
    char     magic[4];       // "ORGB"
    uint32_t device_id;
    uint32_t packet_type;
    uint32_t packet_size;
};
#pragma pack(pop)

enum PacketType : uint32_t {
    REQUEST_CONTROLLER_COUNT = 0,
    REQUEST_CONTROLLER_DATA  = 1,
    REQUEST_PROTOCOL_VERSION = 40,
    SET_CLIENT_NAME          = 50,
    DEVICE_LIST_UPDATED      = 100,
    RGBCONTROLLER_RESIZEZONE = 1000,
    RGBCONTROLLER_UPDATEZONELEDS = 1051,
};

struct Client {
    int sock = -1;
    uint32_t protocol_version = 0;

    std::thread reader_thread;
    std::atomic<bool> running{false};
    std::mutex mtx;
    std::condition_variable cv;

    bool connect(const char* host = "127.0.0.1", uint16_t port = 6742) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { perror("socket"); return false; }

        struct hostent* he = gethostbyname(host);
        if (!he) { close(); return false; }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        struct timeval tv = {3, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect"); close(); return false;
        }

        // Protocol handshake
        uint32_t client_proto = 5;  // we speak v5
        send_packet(0, REQUEST_PROTOCOL_VERSION, &client_proto, 4);

        PacketHeader hdr;
        std::vector<uint8_t> data;
        if (recv_packet(hdr, data) && data.size() >= 4) {
            uint32_t server_proto = *reinterpret_cast<uint32_t*>(data.data());
            protocol_version = (server_proto < client_proto) ? server_proto : client_proto;
            fprintf(stderr, "OpenRGB: negotiated protocol v%u (server v%u)\n", protocol_version, server_proto);
        } else {
            protocol_version = 0;  // fallback for old servers
            fprintf(stderr, "OpenRGB: no version response, using v0 fallback\n");
        }

        // Set short timeout for normal operation
        tv = {1, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Announce client name
        const char* name = "rgb-controller-cpp";
        send_packet(0, SET_CLIENT_NAME, name, strlen(name) + 1);

        // Start background reader to consume server responses
        running = true;
        reader_thread = std::thread([this]() { reader_loop(); });

        return true;
    }

    void close() {
        running = false;
        cv.notify_all();
        if (reader_thread.joinable()) reader_thread.join();
        if (sock >= 0) { ::close(sock); sock = -1; }
    }

    // ── Zone LED update (the only command we need for animation) ──
    // Sends per-LED colors for a zone. Fire-and-forget — does not wait
    // for response. The reader thread consumes the server's reply.
    bool update_zone_leds(uint32_t device_id, uint32_t zone_id,
                          const uint8_t* rgb, uint32_t led_count) {
        // Build data packet:
        // [total_size:4] [zone_id:4] [led_count:2] [R,G,B,0 × N]
        uint32_t data_size = 4 + 4 + 2 + led_count * 4;
        std::vector<uint8_t> buf(data_size);
        *reinterpret_cast<uint32_t*>(&buf[0]) = data_size;
        *reinterpret_cast<uint32_t*>(&buf[4]) = zone_id;
        *reinterpret_cast<uint16_t*>(&buf[8]) = (uint16_t)led_count;
        for (uint32_t i = 0; i < led_count; ++i) {
            buf[10 + i*4]     = rgb[i*3];
            buf[10 + i*4 + 1] = rgb[i*3 + 1];
            buf[10 + i*4 + 2] = rgb[i*3 + 2];
            buf[10 + i*4 + 3] = 0;
        }
        return send_packet(device_id, RGBCONTROLLER_UPDATEZONELEDS, buf.data(), data_size);
    }

    bool resize_zone(uint32_t device_id, uint32_t zone_id, uint32_t size) {
        uint32_t data[2] = {zone_id, size};
        return send_packet(device_id, RGBCONTROLLER_RESIZEZONE, data, 8);
    }

private:
    void reader_loop() {
        // Silently consume all server responses and notifications
        PacketHeader hdr;
        std::vector<uint8_t> data;
        while (running) {
            if (recv_packet(hdr, data)) {
                // DEVICE_LIST_UPDATED notification — just acknowledge
                if (hdr.packet_type == DEVICE_LIST_UPDATED) continue;
                // All other responses are consumed and discarded
            }
            // Timeout or error → loop and retry
        }
    }

    bool write_all(const void* buf, size_t len) {
        size_t total = 0;
        while (total < len) {
            ssize_t n = ::write(sock, (const char*)buf + total, len - total);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    bool read_all(void* buf, size_t len) {
        size_t total = 0;
        while (total < len) {
            ssize_t n = ::read(sock, (char*)buf + total, len - total);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    bool send_packet(uint32_t device_id, uint32_t type,
                     const void* data, uint32_t data_size) {
        PacketHeader hdr{};
        memcpy(hdr.magic, "ORGB", 4);
        hdr.device_id = device_id;
        hdr.packet_type = type;
        hdr.packet_size = data_size;
        return write_all(&hdr, sizeof(hdr)) && (data_size == 0 || write_all(data, data_size));
    }

    bool recv_packet(PacketHeader& hdr, std::vector<uint8_t>& data) {
        if (!read_all(&hdr, sizeof(hdr))) return false;
        if (hdr.packet_size > 0) {
            data.resize(hdr.packet_size);
            return read_all(data.data(), hdr.packet_size);
        }
        return true;
    }
};

} // namespace orgb_client
