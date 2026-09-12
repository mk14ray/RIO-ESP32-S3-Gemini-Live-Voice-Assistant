#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>

// Minimal RFC 6455 client over TLS.
//
// Why not a library:
//   - Links2004/arduinoWebSockets hard-codes WEBSOCKETS_MAX_DATA_SIZE (15 * 1024)
//     as a bare #define (not #ifndef-guarded, so no build flag can raise it), and
//     closes the socket with code 1009 on anything larger. A single 300 ms Gemini
//     reply chunk is ~19 KB of base64 — over the cap.
//   - esp_websocket_client is an ESP-IDF managed component (esp-protocols) and is
//     not shipped in the Arduino-ESP32 core.
//
// This client has no size cap, reassembles fragments into one pre-allocated
// PSRAM buffer (zero per-frame malloc), and hands the caller the raw bytes so a
// base64 span can be decoded straight into the playback buffer.
//
// NOT thread-safe: exactly one task may own an instance.

enum WsEvent : uint8_t {
    WS_CONNECTED,
    WS_DISCONNECTED,
    WS_MESSAGE,     // complete reassembled message; payload/len valid
    WS_OVERFLOW,    // message exceeded the reassembly buffer and was dropped
};

typedef void (*WsCallback)(WsEvent ev, const uint8_t* payload, size_t len, void* user);

class WsClient {
public:
    WsClient();

    /**
     * @param rxBuf caller-owned reassembly buffer (PSRAM), must outlive the client.
     */
    void begin(const char* host, uint16_t port, const char* path,
               const char* rootCaPem, uint8_t* rxBuf, size_t rxCap);

    void onEvent(WsCallback cb, void* user);

    /** TLS connect + HTTP upgrade + Sec-WebSocket-Accept verification. */
    bool connect();

    void disconnect(uint16_t code = 1000);
    bool isConnected() const { return _connected; }

    /** Send one masked text frame. */
    bool sendText(const char* data, size_t len);

    /**
     * Read available frames, reassemble, and fire the callback.
     * Returns false if the connection was lost.
     */
    bool poll(uint32_t maxMs);

    uint32_t connectedForMs() const {
        return _connected ? (millis() - _connectedAt) : 0;
    }

private:
    bool readExact(uint8_t* dst, size_t n, uint32_t timeoutMs);
    bool sendFrame(uint8_t opcode, const uint8_t* payload, size_t len);
    bool readFrame(uint32_t timeoutMs);
    void fail(const char* why);

    WiFiClientSecure _tls;
    const char* _host;
    const char* _path;
    const char* _rootCa;
    uint16_t    _port;

    uint8_t* _rxBuf;
    size_t   _rxCap;
    size_t   _rxLen;        // bytes assembled so far for the current message
    bool     _rxOverflow;   // current message already exceeded the buffer
    uint8_t  _msgOpcode;    // opcode of the in-progress fragmented message

    WsCallback _cb;
    void*      _cbUser;

    bool     _connected;
    uint32_t _connectedAt;
};

#endif  // WS_CLIENT_H
