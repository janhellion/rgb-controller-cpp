#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <sys/select.h>

namespace rgb::openrgb {

// ── OpenRGB SDK Protocol ────────────────────
// TCP-based protocol, port 6742
// Header: "ORGB" + device_id(4) + packet_type(4) + packet_size(4)

#pragma pack(push, 1)
struct PacketHeader {
    char magic[4] = {'O', 'R', 'G', 'B'};
    uint32_t device_id;
    uint32_t packet_type;
    uint32_t packet_size;
};
#pragma pack(pop)

enum PacketType : uint32_t {
    REQUEST_CONTROLLER_COUNT  = 0,
    REQUEST_CONTROLLER_DATA   = 1,
    REQUEST_PROTOCOL_VERSION = 40,
    SET_CLIENT_NAME           = 50,
    RGBCONTROLLER_RESIZEZONE  = 1000,
    RGBCONTROLLER_UPDATELEDS          = 1050,
    RGBCONTROLLER_UPDATEZONELEDS      = 1051,
    RGBCONTROLLER_UPDATESINGLELED     = 1052,
    RGBCONTROLLER_SETMODE             = 1053,
    RGBCONTROLLER_UPDATEMODE          = 1054,
    RGBCONTROLLER_SAVEMODE            = 1055,
};

struct ZoneData {
    std::string name;
    uint32_t led_count;
    uint32_t type;
};

struct DeviceData {
    std::string name;
    uint32_t device_type;
    std::vector<ZoneData> zones;
    uint32_t active_mode;
};

// ── Client ─────────────────────────────────

class Client {
public:
    Client();
    ~Client();

    bool connect(const char* host = "127.0.0.1", uint16_t port = 6742);
    void disconnect();
    bool is_connected() const { return m_sock >= 0; }

    // SDK commands
    uint32_t get_controller_count();
    DeviceData get_controller_data(uint32_t device_id);
    bool set_client_name(const std::string& name);

    // LED control
    bool resize_zone(uint32_t device_id, uint32_t zone_id, uint32_t size);
    bool update_zone_leds(uint32_t device_id, uint32_t zone_id,
                          const std::vector<uint8_t>& rgb_data);
    bool update_single_led(uint32_t device_id, uint32_t led_id,
                           uint8_t r, uint8_t g, uint8_t b);
    bool set_mode(uint32_t device_id, uint32_t mode_id);
    bool recv_any() {
        // Drain one response packet (handles notifications like Python's read())
        PacketHeader hdr;
        std::vector<uint8_t> data;
        if (!recv_packet(hdr, data)) return false;
        // DEVICE_LIST_UPDATED (packet_type=2) means read again
        if (hdr.packet_type == 2) return recv_any();
        return true;
    }

    void drain() {
        // Drain pending responses. First read blocks briefly (resize ack),
        // subsequent reads are non-blocking.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_sock, &fds);
        struct timeval tv = {0, 200000};  // 200ms for first response
        while (select(m_sock + 1, &fds, nullptr, nullptr, &tv) > 0) {
            PacketHeader hdr;
            std::vector<uint8_t> data;
            if (!recv_packet(hdr, data)) break;
            FD_ZERO(&fds);
            FD_SET(m_sock, &fds);
            tv = {0, 10000};  // 10ms for any follow-up
        }
    }

private:
    int m_sock = -1;
    uint32_t m_protocol_version = 0;

    bool send_packet(uint32_t device_id, uint32_t type,
                     const void* data = nullptr, uint32_t data_size = 0);
    bool recv_packet(PacketHeader& header, std::vector<uint8_t>& data);
    bool read_exact(void* buf, size_t len);
    bool write_exact(const void* buf, size_t len);
};

} // namespace rgb::openrgb
