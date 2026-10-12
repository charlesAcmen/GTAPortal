#include "DevTeleport.h"

#include <CGame.h>
#include <CLoadingScreen.h>
#include <CMenuManager.h>
#include <CPlayerPed.h>
#include <CStreaming.h>
#include <CTimeCycle.h>
#include <CCutsceneMgr.h>
#include <common.h>

#include <iomanip>
#include <sstream>

#include "../../DevelopmentAnchors.h"

namespace portal_sa::dev_teleport {
namespace {

const CVector kSpawnPosition{development_anchors::gta_sa::x,
                             development_anchors::gta_sa::y,
                             development_anchors::gta_sa::z};
constexpr float kSpawnHeadingRadians = development_anchors::gta_sa::headingRadians;
constexpr DWORD kReadyDelayMs = 2000;

bool g_teleported = false;
DWORD g_readySince = 0;

bool GameplayReady() {
    return !CLoadingScreen::m_bActive && !FrontEndMenuManager.m_bMenuActive &&
           !CCutsceneMgr::ms_running && FindPlayerPed(0) != nullptr;
}

} // namespace

void OnGameProcess(void (*log)(const std::string&)) {
    if (g_teleported) return;
    if (!GameplayReady()) {
        g_readySince = 0;
        return;
    }

    const DWORD now = GetTickCount();
    if (!g_readySince) {
        g_readySince = now;
        return;
    }
    if (now - g_readySince < kReadyDelayMs) return;

    CPlayerPed* player = FindPlayerPed(0);
    if (!player) return;

    CStreaming::LoadSceneCollision(&kSpawnPosition);
    CStreaming::LoadScene(&kSpawnPosition);
    player->m_nAreaCode = 0;
    CGame::currArea = 0;
    CStreaming::RemoveBuildingsNotInArea(0);
    CTimeCycle::StopExtraColour(false);

    CEntity* entity = player;
    if (player->bInVehicle && player->m_pVehicle) {
        entity = player->m_pVehicle;
        entity->m_nAreaCode = 0;
    }
    entity->Teleport(kSpawnPosition, true);
    entity->SetHeading(kSpawnHeadingRadians);
    g_teleported = true;

    std::ostringstream line;
    line << std::fixed << std::setprecision(6)
         << "SA automatic development teleport completed position=("
         << kSpawnPosition.x << ',' << kSpawnPosition.y << ',' << kSpawnPosition.z
         << ") heading_rad=" << kSpawnHeadingRadians;
    log(line.str());
}

} // namespace portal_sa::dev_teleport
