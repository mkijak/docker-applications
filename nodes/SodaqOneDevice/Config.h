/*
Copyright (c) 2016, SODAQ
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdint.h>
#include <Arduino.h>

typedef void(*VoidCallbackMethodPtr)(void);

struct ConfigParams
{
    uint16_t _header;

    uint16_t _defaultFixInterval;
    uint16_t _alternativeFixInterval;
    uint8_t _alternativeFixFromHours;
    uint8_t _alternativeFixFromMinutes;
    uint8_t _alternativeFixToHours;
    uint8_t _alternativeFixToMinutes;
    uint16_t _gpsFixTimeout;

    uint8_t _isLedEnabled;
    uint8_t _isOtaaEnabled;
    uint8_t _isGpsEnabled;
    uint8_t _isAckEnabled;
    uint8_t _isDbgEnabled;
    
    char _devAddrOrEUI[16 + 1];
    char _appSKeyOrEUI[32 + 1];
    char _nwSKeyOrAppKey[32 + 1];

    uint32_t _chargeOffset;

    uint8_t _coordinateUploadCount;
    uint8_t _repeatCount;
    uint32_t _frameUpCount;
    uint32_t _frameDownCount;

    uint16_t _crc16;

public:
    void read();
    void commit(bool forced = false);
    void reset();

    bool execCommand(const char* line);

    uint16_t getDefaultFixInterval() const { return _defaultFixInterval; }
    uint16_t getAlternativeFixInterval() const { return _alternativeFixInterval; }
    uint8_t getAlternativeFixFromHours() const { return _alternativeFixFromHours; }
    uint8_t getAlternativeFixFromMinutes() const { return _alternativeFixFromMinutes; }
    uint32_t getAlternativeFixFrom() const { return _alternativeFixFromHours * 60 * 60 + _alternativeFixFromMinutes * 60; }
    uint8_t getAlternativeFixToHours() const { return _alternativeFixToHours; }
    uint8_t getAlternativeFixToMinutes() const { return _alternativeFixToMinutes; }
    uint32_t getAlternativeFixTo() const { return _alternativeFixToHours * 60 * 60 + _alternativeFixToMinutes * 60; }
    uint16_t getGpsFixTimeout() const { return _gpsFixTimeout; }

    uint8_t getIsLedEnabled() const { return _isLedEnabled; }
    uint8_t getIsOtaaEnabled() const { return _isOtaaEnabled; }
    uint8_t getIsGpsEnabled() const { return _isGpsEnabled; }
    uint8_t getIsAckEnabled() const { return _isAckEnabled; }
    uint8_t getIsDbgEnabled() const { return _isDbgEnabled; }
    
    const char* getDevAddrOrEUI() const { return _devAddrOrEUI; }
    const char* getAppSKeyOrEUI() const { return _appSKeyOrEUI; }
    const char* getNwSKeyOrAppKey() const { return _nwSKeyOrAppKey; }

    uint32_t getChargeOffset() const { return _chargeOffset; }
    void     setChargeOffset(uint32_t offset) { _chargeOffset = offset; }
    
    uint8_t getCoordinateUploadCount() const { return _coordinateUploadCount; }
    uint8_t getRepeatCount() const { return _repeatCount; }
    uint32_t getFrameUpCount() const { return _frameUpCount; }
    uint32_t getFrameDownCount() const { return _frameDownCount; }

    static void showConfig(Stream* stream);
    bool checkConfig(Stream& stream);
    void setConfigResetCallback(VoidCallbackMethodPtr callback);
};

extern ConfigParams params;

#endif
