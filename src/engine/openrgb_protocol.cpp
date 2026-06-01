#include "openrgb_protocol.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace rgb::openrgb {

Client::Client() {}
Client::~Client() { disconnect(); }

bool Client::connect(const char* host, uint16_t port) {
    m_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_sock < 0) return false;

    struct hostent* he = gethostbyname(host);
    if (!he) { disconnect(); return false; }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (::connect(m_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        disconnect();
        return false;
    }

    // Set client name
    set_client_name("rgb-controller-cpp");
    return true;
}

void Client::disconnect() {
    if (m_sock >= 0) {
        ::close(m_sock);
        m_sock = -1;
    }
}

bool Client::read_exact(void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::read(m_sock, (char*)buf + total, len - total);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool Client::write_exact(const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::write(m_sock, (const char*)buf + total, len - total);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool Client::send_packet(uint32_t device_id, uint32_t type,
                          const void* data, uint32_t data_size) {
    PacketHeader hdr{};
    hdr.device_id = device_id;
    hdr.packet_type = type;
    hdr.packet_size = data_size;

    if (!write_exact(&hdr, sizeof(hdr))) return false;
    if (data_size > 0 && data) {
        if (!write_exact(data, data_size)) return false;
    }
    return true;
}

bool Client::recv_packet(PacketHeader& header, std::vector<uint8_t>& data) {
    if (!read_exact(&header, sizeof(header))) return false;
    if (header.packet_size > 0) {
        data.resize(header.packet_size);
        if (!read_exact(data.data(), header.packet_size)) return false;
    }
    return true;
}

bool Client::set_client_name(const std::string& name) {
    return send_packet(0, SET_CLIENT_NAME, name.c_str(), name.size() + 1);
}

uint32_t Client::get_controller_count() {
    if (!send_packet(0, REQUEST_CONTROLLER_COUNT)) return 0;
    PacketHeader hdr;
    std::vector<uint8_t> data;
    if (!recv_packet(hdr, data)) return 0;
    if (data.size() >= 4) return *reinterpret_cast<uint32_t*>(data.data());
    return 0;
}

DeviceData Client::get_controller_data(uint32_t device_id) {
    DeviceData dev{};
    uint32_t proto = m_protocol_version;
    if (!send_packet(device_id, REQUEST_CONTROLLER_DATA, &proto, 4)) {
        return dev;
    }

    PacketHeader hdr;
    std::vector<uint8_t> data;
    if (!recv_packet(hdr, data)) return dev;

    // Parse controller data (simplified)
    // In production, parse the full OpenRGB controller data structure
    size_t pos = 0;
    if (pos + 4 > data.size()) return dev;
    uint32_t name_len = *reinterpret_cast<uint32_t*>(data.data() + pos); pos += 4;
    dev.name = std::string((char*)(data.data() + pos), name_len); pos += name_len;
    if (pos + 4 > data.size()) return dev;
    dev.device_type = *reinterpret_cast<uint32_t*>(data.data() + pos); pos += 4;

    uint32_t zone_count = *reinterpret_cast<uint32_t*>(data.data() + pos); pos += 4;
    for (uint32_t z = 0; z < zone_count && z < 16; ++z) {
        if (pos + 4 > data.size()) break;
        uint32_t zname_len = *reinterpret_cast<uint32_t*>(data.data() + pos); pos += 4;
        ZoneData zd{};
        zd.name = std::string((char*)(data.data() + pos), zname_len); pos += zname_len;
        if (pos + 4 > data.size()) break;
        zd.type = *reinterpret_cast<uint32_t*>(data.data() + pos); pos += 4;
        if (pos + 4 > data.size()) break;
        zd.led_count = *reinterpret_cast<uint32_t*>(data.data() + pos); pos += 4;
        // Skip LEDs data
        uint32_t leds_count = *reinterpret_cast<uint32_t*>(data.data() + pos); pos += 4;
        for (uint32_t li = 0; li < leds_count && li < 256; ++li) {
            if (pos + 8 > data.size()) break;
            uint32_t lname_len = *reinterpret_cast<uint32_t*>(data.data() + pos); pos += 4;
            pos += lname_len;  // skip LED name
            pos += 4;          // skip LED color
        }
        dev.zones.push_back(zd);
    }

    return dev;
}

bool Client::resize_zone(uint32_t device_id, uint32_t zone_id, uint32_t size) {
    uint32_t data[2] = {zone_id, size};
    return send_packet(device_id, RGBCONTROLLER_RESIZEZONE, data, 8);
}

bool Client::update_zone_leds(uint32_t device_id, uint32_t zone_id,
                               const std::vector<uint8_t>& rgb_data) {
    // Pack: zone_id(4) + led_count(2) + num_colors(4) + RGB data
    uint32_t led_count = rgb_data.size() / 3;
    uint32_t total_size = 4 + 2 + 4 + rgb_data.size();
    std::vector<uint8_t> packet(total_size);

    *reinterpret_cast<uint32_t*>(&packet[0]) = zone_id;
    *reinterpret_cast<uint16_t*>(&packet[4]) = led_count;
    *reinterpret_cast<uint32_t*>(&packet[6]) = total_size;
    memcpy(&packet[10], rgb_data.data(), rgb_data.size());

    return send_packet(device_id, RGBCONTROLLER_UPDATEZONELEDS,
                       packet.data(), packet.size());
}

bool Client::update_single_led(uint32_t device_id, uint32_t led_id,
                                uint8_t r, uint8_t g, uint8_t b) {
    uint8_t data[8] = {};
    *reinterpret_cast<uint32_t*>(data) = led_id;
    data[4] = r; data[5] = g; data[6] = b;
    return send_packet(device_id, RGBCONTROLLER_UPDATESINGLELED, data, 8);
}

bool Client::set_mode(uint32_t device_id, uint32_t mode_id) {
    return send_packet(device_id, RGBCONTROLLER_SETMODE, &mode_id, 4);
}

} // namespace rgb::openrgb
