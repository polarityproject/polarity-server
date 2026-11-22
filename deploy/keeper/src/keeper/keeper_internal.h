#pragma once
// What the keeper's files share: its rooms, tables, timings and helpers (docs/keeper.md).

#include "keeper/keeper.h"

#include "core/lobby.h"
#include "core/rules.h"

#include <weyvelength.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sfv::keeper {

} // namespace sfv::keeper

namespace sfv::keeper::internal {

std::string Str(const char* p, uint32_t len);

constexpr uint8_t kControlChannel = 1;

void SendLobby(WeyveClient* c, uint64_t to, const lobby::Message& m);

const char* RoomErrorText(WeyveRoomError e);

constexpr int64_t kResendMs = 250;

constexpr int64_t kAnswerForMs = 20000;

constexpr int64_t kOwnerWaitMs = 30000;

constexpr int64_t kEmptyCloseMs = 3000;

constexpr int64_t kListEveryMs = 2000;

constexpr int64_t kRoomAskEveryMs = 10000;

constexpr int64_t kPeekEveryMs = 500;

constexpr int kCallSeconds = 21;

constexpr int kBrokeOffCallSeconds = 26;

constexpr int64_t kConnectMs = 45000;

constexpr int64_t kAfterMs = 5000;

} // namespace sfv::keeper::internal
