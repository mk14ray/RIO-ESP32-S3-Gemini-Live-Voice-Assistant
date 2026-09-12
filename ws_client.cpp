#include "ws_client.h"
#include "b64.h"
#include "config.h"
#include <mbedtls/sha1.h>
#include <esp_random.h>

// RFC 6455 opcodes
#define OP_CONT   0x0
#define OP_TEXT   0x1
#define OP_BIN    0x2
#define OP_CLOSE  0x8
#define OP_PING   0x9
#define OP_PONG   0xA

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

WsClient::WsClient()
    : _host(nullptr), _path(nullptr), _rootCa(nullptr), _port(443),
      _rxBuf(nullptr), _rxCap(0), _rxLen(0), _rxOverflow(false), _msgOpcode(0),
      _cb(nullptr), _cbUser(nullptr), _connected(false), _connectedAt(0) {}

void WsClient::begin(const char* host, uint16_t port, const char* path,
                     const char* rootCaPem, uint8_t* rxBuf, size_t rxCap) {
    _host   = host;
    _port   = port;
    _path   = path;
    _rootCa = rootCaPem;
    _rxBuf  = rxBuf;
    _rxCap  = rxCap;
}

void WsClient::onEvent(WsCallback cb, void* user) {
    _cb     = cb;
    _cbUser = user;
}

void WsClient::fail(const char* why) {
    Serial.printf("[WS] %s\n", why);
    _tls.stop();
    if (_connected) {
        _connected = false;
        if (_cb) {
            _cb(WS_DISCONNECTED, nullptr, 0, _cbUser);
        }
    }
}

bool WsClient::connect() {
    _rxLen      = 0;
    _rxOverflow = false;
    _msgOpcode  = 0;

    if (_rootCa != nullptr) {
        _tls.setCACert(_rootCa);
    } else {
        _tls.setInsecure();   // debugging only
    }
    _tls.setTimeout(15);      // seconds — applies to the underlying socket

    Serial.printf("[WS] connecting to %s:%u ...\n", _host, _port);
    if (!_tls.connect(_host, _port)) {
        Serial.println("[WS] TLS connect failed (check WiFi, time, or CA cert)");
        return false;
    }

    // --- Build the handshake -------------------------------------------------
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));

    char keyB64[32];
    size_t keyLen = 0;
    if (!b64Encode(nonce, sizeof(nonce), keyB64, sizeof(keyB64), &keyLen)) {
        fail("failed to encode Sec-WebSocket-Key");
        return false;
    }

    String req;
    req.reserve(512);
    req  = "GET ";
    req += _path;
    req += " HTTP/1.1\r\n";
    req += "Host: ";        req += _host;   req += "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: ";     req += keyB64; req += "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "\r\n";

    if (_tls.print(req) == 0) {
        fail("failed to send handshake");
        return false;
    }

    // --- Read the response ---------------------------------------------------
    String statusLine;
    String acceptHdr;
    bool   sawStatus = false;
    uint32_t start   = millis();

    while (millis() - start < 15000) {
        if (!_tls.connected() && !_tls.available()) {
            fail("connection closed during handshake");
            return false;
        }
        if (!_tls.available()) {
            delay(5);
            continue;
        }

        String line = _tls.readStringUntil('\n');
        line.trim();

        if (!sawStatus) {
            statusLine = line;
            sawStatus  = true;
            continue;
        }
        if (line.length() == 0) {
            break;   // end of headers
        }

        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith("sec-websocket-accept:")) {
            acceptHdr = line.substring(line.indexOf(':') + 1);
            acceptHdr.trim();
        }
    }

    if (statusLine.indexOf("101") < 0) {
        Serial.printf("[WS] upgrade rejected: %s\n", statusLine.c_str());
        // Google returns the reason in the body — surface it, it is usually
        // "API key not valid" or a bad model name.
        String body;
        uint32_t t0 = millis();
        while (millis() - t0 < 1000 && body.length() < 512) {
            while (_tls.available() && body.length() < 512) {
                body += (char)_tls.read();
            }
            delay(10);
        }
        if (body.length()) {
            Serial.printf("[WS] body: %s\n", body.c_str());
        }
        _tls.stop();
        return false;
    }

    // --- Verify Sec-WebSocket-Accept ----------------------------------------
    // base64(sha1(key + GUID)). Cheap, and turns a whole class of silent
    // misconfiguration into one clear log line.
    {
        String concat = String(keyB64) + WS_GUID;
        uint8_t digest[20];
        mbedtls_sha1((const unsigned char*)concat.c_str(), concat.length(), digest);

        char expect[32];
        size_t expectLen = 0;
        if (!b64Encode(digest, sizeof(digest), expect, sizeof(expect), &expectLen)) {
            fail("failed to compute expected accept token");
            return false;
        }

        if (acceptHdr != expect) {
            Serial.printf("[WS] bad Sec-WebSocket-Accept (got '%s', want '%s')\n",
                          acceptHdr.c_str(), expect);
            _tls.stop();
            return false;
        }
    }

    _connected   = true;
    _connectedAt = millis();
    Serial.println("[WS] connected");
    if (_cb) {
        _cb(WS_CONNECTED, nullptr, 0, _cbUser);
    }
    return true;
}

void WsClient::disconnect(uint16_t code) {
    if (_connected) {
        uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
        sendFrame(OP_CLOSE, payload, sizeof(payload));
    }
    _tls.stop();
    if (_connected) {
        _connected = false;
        if (_cb) {
            _cb(WS_DISCONNECTED, nullptr, 0, _cbUser);
        }
    }
}

bool WsClient::sendFrame(uint8_t opcode, const uint8_t* payload, size_t len) {
    if (!_tls.connected()) {
        return false;
    }

    uint8_t hdr[14];
    size_t  h = 0;

    hdr[h++] = 0x80 | (opcode & 0x0F);          // FIN + opcode

    // Every client->server frame MUST be masked (RFC 6455 §5.3); servers close
    // the connection otherwise.
    if (len < 126) {
        hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xFFFF) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)(len & 0xFF);
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) {
            hdr[h++] = (uint8_t)(((uint64_t)len >> (8 * i)) & 0xFF);
        }
    }

    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    memcpy(hdr + h, mask, 4);
    h += 4;

    if (_tls.write(hdr, h) != h) {
        fail("header write failed");
        return false;
    }

    // Mask in blocks so we never need a full-size copy of the payload.
    uint8_t block[512];
    size_t  off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > sizeof(block)) {
            n = sizeof(block);
        }
        for (size_t i = 0; i < n; i++) {
            block[i] = payload[off + i] ^ mask[(off + i) & 3];
        }
        if (_tls.write(block, n) != n) {
            fail("payload write failed");
            return false;
        }
        off += n;
    }

    return true;
}

bool WsClient::sendText(const char* data, size_t len) {
    bool ok = sendFrame(OP_TEXT, (const uint8_t*)data, len);
    if (ok) {
#if LOG_WS_FRAMES
        Serial.printf("[WS] --> tx %u bytes\n", (unsigned)len);
#endif
    }
    return ok;
}

bool WsClient::readExact(uint8_t* dst, size_t n, uint32_t timeoutMs) {
    size_t   got   = 0;
    uint32_t start = millis();

    while (got < n) {
        if (millis() - start > timeoutMs) {
            return false;
        }
        int avail = _tls.available();
        if (avail <= 0) {
            if (!_tls.connected()) {
                return false;
            }
            delay(2);
            continue;
        }
        int r = _tls.read(dst + got, n - got);
        if (r > 0) {
            got += (size_t)r;
            start = millis();   // reset the clock on progress
        } else if (r < 0) {
            return false;
        }
    }
    return true;
}

bool WsClient::readFrame(uint32_t timeoutMs) {
    uint8_t b[2];
    if (!readExact(b, 2, timeoutMs)) {
        return false;
    }

    const bool    fin    = (b[0] & 0x80) != 0;
    const uint8_t opcode = b[0] & 0x0F;
    const bool    masked = (b[1] & 0x80) != 0;   // must be 0 from a server
    uint64_t      len    = b[1] & 0x7F;

    if (len == 126) {
        uint8_t e[2];
        if (!readExact(e, 2, timeoutMs)) return false;
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        uint8_t e[8];
        if (!readExact(e, 8, timeoutMs)) return false;
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | e[i];
        }
    }

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked && !readExact(mask, 4, timeoutMs)) {
        return false;
    }

    // --- Control frames: never fragmented, max 125 bytes ---------------------
    if (opcode == OP_PING || opcode == OP_PONG || opcode == OP_CLOSE) {
        uint8_t ctrl[125];
        size_t  n = (size_t)(len > sizeof(ctrl) ? sizeof(ctrl) : len);
        if (n > 0 && !readExact(ctrl, n, timeoutMs)) {
            return false;
        }
        // Drain any excess so the stream stays aligned.
        for (uint64_t i = n; i < len; i++) {
            uint8_t junk;
            if (!readExact(&junk, 1, timeoutMs)) return false;
        }
        if (masked) {
            for (size_t i = 0; i < n; i++) {
                ctrl[i] ^= mask[i & 3];
            }
        }

        if (opcode == OP_PING) {
            sendFrame(OP_PONG, ctrl, n);   // must echo the payload verbatim
        } else if (opcode == OP_CLOSE) {
            uint16_t code = (n >= 2) ? (uint16_t)((ctrl[0] << 8) | ctrl[1]) : 1005;
            Serial.printf("[WS] server close, code=%u\n", code);
            sendFrame(OP_CLOSE, ctrl, n);
            fail("closed by server");
            return false;
        }
        return true;
    }

    // --- Data frames ---------------------------------------------------------
    if (opcode != OP_CONT) {
        _msgOpcode  = opcode;
        _rxLen      = 0;
        _rxOverflow = false;
    }

    // Read the payload, appending while it fits. On overflow we KEEP CONSUMING
    // to the end of the message: abandoning mid-frame desynchronises the stream
    // and every subsequent byte would be garbage.
    uint64_t remaining = len;
    uint64_t consumed  = 0;
    uint8_t  sink[512];

    while (remaining > 0) {
        size_t   want = (size_t)(remaining > sizeof(sink) ? sizeof(sink) : remaining);
        uint8_t* dst;
        bool     intoBuf = (!_rxOverflow && _rxLen + want <= _rxCap);

        if (intoBuf) {
            dst = _rxBuf + _rxLen;
        } else {
            if (!_rxOverflow) {
                _rxOverflow = true;
                Serial.printf("[WS] message exceeds %u byte buffer — dropping\n",
                              (unsigned)_rxCap);
            }
            dst = sink;
        }

        if (!readExact(dst, want, timeoutMs)) {
            return false;
        }

        if (masked) {
            for (size_t i = 0; i < want; i++) {
                dst[i] ^= mask[(size_t)((consumed + i) & 3)];
            }
        }

        if (intoBuf) {
            _rxLen += want;
        }
        consumed  += want;
        remaining -= want;
    }

    if (fin) {
        if (_rxOverflow) {
            Serial.println("[WS] <-- rx OVERFLOW (message dropped)");
            if (_cb) {
                _cb(WS_OVERFLOW, nullptr, 0, _cbUser);
            }
        } else {
#if LOG_WS_FRAMES
            Serial.printf("[WS] <-- rx %u bytes\n", (unsigned)_rxLen);
#endif
            if (_cb) {
                _cb(WS_MESSAGE, _rxBuf, _rxLen, _cbUser);
            }
        }
        _rxLen      = 0;
        _rxOverflow = false;
    }

    return true;
}

bool WsClient::poll(uint32_t maxMs) {
    if (!_connected) {
        return false;
    }
    if (!_tls.connected() && !_tls.available()) {
        fail("connection lost");
        return false;
    }

    uint32_t start = millis();
    while (_tls.available() > 0) {
        if (!readFrame(10000)) {
            return _connected;   // readFrame reports/handles the failure
        }
        if (millis() - start >= maxMs) {
            break;   // yield; remaining frames are picked up next poll
        }
    }
    return true;
}
