// sceNp / cellNetCtl / sys_net: a consistently *offline* PlayStation Network.
// The PC port never signs in: status is OFFLINE, identity/ticket queries fail
// with SCE_NP_ERROR_OFFLINE, lists are empty, the network is disconnected.
#include "../kernel/kernel.h"

namespace {
enum : uint32_t {
    SCE_NP_ERROR_OFFLINE = 0x8002AA0C,
    SCE_NP_BASIC_ERROR_NO_EVENT = 0x80022B07,
    CELL_NET_CTL_ERROR_NOT_CONNECTED = 0x80130106,
};
constexpr int32_t SCE_NP_MANAGER_STATUS_OFFLINE = -1;
}  // namespace

// ---- library lifetime / callbacks
HLE_FUNC(sceNp, sceNpInit) {
    LOG_I("np", "sceNpInit: PSN is reported as offline");
    RET(CELL_OK);
}
HLE_FUNC(sceNp, sceNpTerm) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpManagerRegisterCallback) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpManagerUnregisterCallback) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpBasicRegisterHandler) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpBasicRegisterContextSensitiveHandler) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpBasicUnregisterHandler) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpDrmIsAvailable) { RET(CELL_OK); }

// ---- manager: status and identity
HLE_FUNC(sceNp, sceNpManagerGetStatus) {
    vm::write32(ARG32(0), (uint32_t)SCE_NP_MANAGER_STATUS_OFFLINE);
    RET(CELL_OK);
}
HLE_FUNC(sceNp, sceNpManagerGetNpId) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetOnlineId) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetOnlineName) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetPsHandle) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetSigninId) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetAccountRegion) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetAccountAge) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetMyLanguages) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetTicket) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetTicketParam) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerRequestTicket2) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetEntitlementIdList) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpManagerGetEntitlementById) { RET(SCE_NP_ERROR_OFFLINE); }
// (isRestricted*, age*): no parental restriction
HLE_FUNC(sceNp, sceNpManagerGetContentRatingFlag) {
    if (ARG32(0)) vm::write32(ARG32(0), 0);
    if (ARG32(1)) vm::write32(ARG32(1), 18);
    RET(CELL_OK);
}
HLE_FUNC(sceNp, sceNpManagerGetChatRestrictionFlag) {
    if (ARG32(0)) vm::write32(ARG32(0), 0);
    RET(CELL_OK);
}

// ---- basic: presence, friends, events
HLE_FUNC(sceNp, sceNpBasicSetPresence) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpBasicSetPresenceDetails) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpBasicAddPlayersHistoryAsync) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpBasicGetFriendListEntryCount) {
    vm::write32(ARG32(0), 0);
    RET(CELL_OK);
}
HLE_FUNC(sceNp, sceNpBasicGetBlockListEntryCount) {
    vm::write32(ARG32(0), 0);
    RET(CELL_OK);
}
HLE_FUNC(sceNp, sceNpBasicGetFriendListEntry) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpBasicGetBlockListEntry) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpBasicGetEvent) { RET(SCE_NP_BASIC_ERROR_NO_EVENT); }

// ---- lookup (online only)
HLE_FUNC(sceNp, sceNpLookupInit) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpLookupTerm) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpLookupCreateTitleCtx) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpLookupDestroyTitleCtx) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpLookupCreateTransactionCtx) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpLookupDestroyTransactionCtx) { RET(CELL_OK); }
HLE_FUNC(sceNp, sceNpLookupNpIdAsync) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpLookupPollAsync) { RET(SCE_NP_ERROR_OFFLINE); }
HLE_FUNC(sceNp, sceNpLookupAbortTransaction) { RET(CELL_OK); }

// ---- network control: disconnected
HLE_FUNC(cellNetCtl, cellNetCtlInit) { RET(CELL_OK); }
HLE_FUNC(cellNetCtl, cellNetCtlTerm) { RET(CELL_OK); }
HLE_FUNC(cellNetCtl, cellNetCtlGetState) {
    vm::write32(ARG32(0), 0);  // CELL_NET_CTL_STATE_Disconnected
    RET(CELL_OK);
}
HLE_FUNC(cellNetCtl, cellNetCtlGetInfo) { RET(CELL_NET_CTL_ERROR_NOT_CONNECTED); }
HLE_FUNC(cellNetCtl, cellNetCtlGetNatInfo) { RET(CELL_NET_CTL_ERROR_NOT_CONNECTED); }
HLE_FUNC(cellNetCtl, cellNetCtlAddHandler) {
    if (ARG32(2)) vm::write32(ARG32(2), 1);  // handler id
    RET(CELL_OK);
}
HLE_FUNC(cellNetCtl, cellNetCtlDelHandler) { RET(CELL_OK); }
HLE_FUNC(sys_net, sys_net_initialize_network_ex) { RET(CELL_OK); }
HLE_FUNC(sys_net, sys_net_finalize_network) { RET(CELL_OK); }
