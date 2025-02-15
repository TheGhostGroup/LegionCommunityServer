/*
 * Copyright (C) 2008-2018 TrinityCore <https://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _WORLDPACKETCRYPT_H
#define _WORLDPACKETCRYPT_H

#include "AuthDefines.h"
#include "Cryptography/ARC4.h"

class BigNumber;


class TC_COMMON_API WorldPacketCrypt
{
public:
    WorldPacketCrypt();

    void Init(SessionKey const& K);
    void Init(SessionKey const& K, std::array<uint8, 16> const& serverKey, std::array<uint8, 16> const& clientKey);
    void DecryptRecv(uint8* data, size_t len);
    void EncryptSend(uint8* data, size_t len);

    bool IsInitialized() const { return _initialized; }

private:
    Trinity::Crypto::ARC4 _clientDecrypt;
    Trinity::Crypto::ARC4 _serverEncrypt;
    bool _initialized;
};

#endif // _WORLDPACKETCRYPT_H
