// Online features on the host, as a console that is offline: no BCAT delivery cache (event quests come from the
// game files / mods instead), no friends, applets (error, web, shop) that close at once. Each call is logged once.
#include <string.h>

#include "../hle.h"

#define ONCE(...) do { static int once_; if (!once_++) LOG(__VA_ARGS__); } while (0)

// Failure results the SDK uses for "not available": callers only check for success.
#define BCAT_NOT_AVAILABLE MAKE_RESULT(122, 1)
#define FRIENDS_NOT_AVAILABLE MAKE_RESULT(121, 2)

// ---- nn::bcat
HLE_FUNC("_ZN2nn4bcat10InitializeEv", bcat_initialize) { (void)c; ONCE("[offline] bcat: no delivery cache\n"); }
HLE_FUNC("_ZN2nn4bcat25MountDeliveryCacheStorageEv", bcat_mount) { RET(BCAT_NOT_AVAILABLE); }
HLE_FUNC("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", bcat_unmount) { (void)c; }
// DeliveryCacheProgress {impl *, ...}: kept zeroed; Status Done (9) with a failed result
HLE_FUNC("_ZN2nn4bcat21DeliveryCacheProgressC1Ev", bcat_progress_ctor) { memset(ARGP(0), 0, 0x20); }
HLE_FUNC("_ZN2nn4bcat21DeliveryCacheProgressD1Ev", bcat_progress_dtor) { (void)c; }
HLE_FUNC("_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv", bcat_progress_update) { (void)c; }
HLE_FUNC("_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv", bcat_progress_status) { RET(9); }
HLE_FUNC("_ZNK2nn4bcat21DeliveryCacheProgress9GetResultEv", bcat_progress_result) { RET(BCAT_NOT_AVAILABLE); }
HLE_FUNC("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", bcat_request_sync) { RET(BCAT_NOT_AVAILABLE); }
HLE_FUNC("_ZN2nn4bcat17DeliveryCacheFileC1Ev", bcat_file_ctor) { memset(ARGP(0), 0, 0x10); }
HLE_FUNC("_ZN2nn4bcat17DeliveryCacheFileD1Ev", bcat_file_dtor) { (void)c; }
HLE_FUNC("_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc", bcat_file_open) { RET(BCAT_NOT_AVAILABLE); }
HLE_FUNC("_ZN2nn4bcat17DeliveryCacheFile4ReadEPjxPvj", bcat_file_read) { if (ARG(1)) ST32(ARG(1), 0); RET(BCAT_NOT_AVAILABLE); }
HLE_FUNC("_ZN2nn4bcat17DeliveryCacheFile5CloseEv", bcat_file_close) { (void)c; }

// ---- nn::friends
HLE_FUNC("_ZN2nn7friends10InitializeEv", friends_initialize) { (void)c; ONCE("[offline] friends: none\n"); }
HLE_FUNC("_ZN2nn7friends12SetAllocatorEPFPvjEPFvS1_jE", friends_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nn7friends12UserPresenceC1Ev", friends_presence_ctor) { memset(ARGP(0), 0, 0x20); }
HLE_FUNC("_ZN2nn7friends12UserPresence10InitializeEv", friends_presence_init) { RET(0); }
HLE_FUNC("_ZN2nn7friends12UserPresence11SetAppValueEPKcS3_", friends_presence_set) { RET(0); }
HLE_FUNC("_ZN2nn7friends12UserPresence6CommitEv", friends_presence_commit) { RET(0); }
// GetFriendList(int *count, ..., offset, max, const FriendFilter &): the friends on the hub server who list us back
// (runtime/net/ldn.c: their account ids, which their games report), so a hub's "everyone but friends" passcode lets
// them in. MHGU asks for the Friend records. nn::friends::Friend (0x200 bytes, as the SDK's accessors read it):
// +0x00 the user's Uid, +0x10 the friend's NetworkServiceAccountId, +0x18 nickname[0x21], +0x40 UserPresence,
// +0x120 favorite, +0x121 newly added, +0x128 valid. The filter is not applied (every friend counts).
#define FRIEND_SIZE 0x200
extern const uint8_t g_user_id[16];  // (services/misc.c)
int ldn_game_friends(uint64_t *ids, char (*names)[33], int max);
static int friends_given(int k, const char *which) {
    static int last[2] = {-1, -1};
    int i = which[0] == 'r';
    if (k != last[i]) LOG("[offline] friends: GetFriendList (%s): %d friend(s) given\n", which, k), last[i] = k;
    return k;
}
HLE_FUNC("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", friends_list) {
    uint64_t ids[64];
    char names[64][33];
    int n = ldn_game_friends(ids, names, 64), offset = (int)ARG(2), max = (int)ARG(3), k = 0;
    for (int i = offset < 0 ? 0 : offset; i < n && k < max; i++, k++) {
        uint32_t f = ARG(1) + (uint32_t)k * FRIEND_SIZE;
        uint8_t *p = G2H(f);
        memset(p, 0, FRIEND_SIZE);
        memcpy(p, g_user_id, 16);
        memcpy(p + 0x10, &ids[i], 8);
        snprintf((char *)p + 0x18, 0x21, "%s", names[i]);
        p[0x128] = 1;  // valid
    }
    if (ARG(0)) ST32(ARG(0), k);
    friends_given(k, "records");
    RET(0);
}
HLE_FUNC("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdEiiRKNS0_12FriendFilterE", friends_list_ids) {
    int ldn_friend_account_ids(uint64_t *out, int max);
    uint64_t ids[64];
    int n = ldn_friend_account_ids(ids, 64), offset = (int)ARG(2), max = (int)ARG(3), k = 0;
    for (int i = offset < 0 ? 0 : offset; i < n && k < max; i++, k++) ST64(ARG(1) + 8u * (uint32_t)k, ids[i]);
    if (ARG(0)) ST32(ARG(0), k);
    friends_given(k, "ids");
    RET(0);
}
HLE_FUNC("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendEPKNS_7account23NetworkServiceAccountIdEi", friends_update) { RET(0); }
HLE_FUNC("_ZN2nn7friends6FriendC1Ev", friends_friend_ctor) { memset(ARGP(0), 0, FRIEND_SIZE); }
HLE_FUNC("_ZNK2nn7friends6Friend7IsValidEv", friends_friend_valid) { RET(LD8(ARG(0) + 0x128)); }
HLE_FUNC("_ZN2nn7friends12AsyncContextC1Ev", friends_async_ctor) { memset(ARGP(0), 0, 0x10); }
HLE_FUNC("_ZN2nn7friends12AsyncContextD1Ev", friends_async_dtor) { (void)c; }
HLE_FUNC("_ZN2nn7friends12AsyncContext7HasDoneEPb", friends_async_done) { if (ARG(1)) ST8(ARG(1), 1); RET(0); }
HLE_FUNC("_ZN2nn7friends17SendFriendRequestEPNS0_12AsyncContextERKNS_7account3UidENS3_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", friends_send_req) {
    RET(FRIENDS_NOT_AVAILABLE);
}
HLE_FUNC("_ZN2nn7friends36GetFacedFriendRequestRegistrationKeyEPNS0_33FacedFriendRequestRegistrationKeyERKNS_7account3UidE", friends_faced_key) {
    if (ARG(0)) memset(ARGP(0), 0, 0x40);
    RET(0);
}
HLE_FUNC("_ZN2nn7friends21AddFacedFriendRequestERKNS_7account3UidERKNS0_33FacedFriendRequestRegistrationKeyERKNS1_8NicknameEPKvjRKNS0_15InAppScreenNameESF_", friends_add_faced) {
    RET(0);
}
HLE_FUNC("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", friends_history_key) {
    if (ARG(0)) memset(ARGP(0), 0, 0x40);
    RET(0);
}
HLE_FUNC("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", friends_add_history) {
    RET(0);
}
HLE_FUNC("_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", friends_open_session) { RET(0); }
HLE_FUNC("_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", friends_close_session) { RET(0); }

// ---- applets: error / web / shop dialogs close at once (logged)
HLE_FUNC("_ZN2nn3err9ShowErrorENS_6ResultE", err_show_result) { LOG("[offline] error dialog: result %#x\n", ARG(0)); }
HLE_FUNC("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", err_show_code) { LOG("[offline] error dialog: %u-%u\n", ARG(0), ARG(1)); }
HLE_FUNC("_ZN2nn3err23ShowNetworkServiceErrorENS0_9ErrorCodeE", err_show_net) { LOG("[offline] network error dialog: %u-%u\n", ARG(0), ARG(1)); }
HLE_FUNC("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", err_show_app) { (void)c; LOG("[offline] application error dialog\n"); }
HLE_FUNC("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", web_show) {
    if (ARG(0)) memset(ARGP(0), 0, 0x10);  // exit reason: ended by the user
    LOG("[offline] web page not shown\n");
    RET(0);
}
HLE_FUNC("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", web_show_offline) {
    if (ARG(0)) memset(ARGP(0), 0, 0x10);
    LOG("[offline] offline html page not shown\n");
    RET(0);
}
HLE_FUNC("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdE", ec_show_shop) { (void)c; LOG("[offline] shop page not shown\n"); }

// ---- nn::nifm: no internet connection (the game's Online Play entries then report that and stay offline)
#define NIFM_NOT_AVAILABLE MAKE_RESULT(110, 300)
HLE_FUNC("_ZN2nn4nifm10InitializeEv", nifm_initialize) { RET(0); }
HLE_FUNC("_ZN2nn4nifm18IsNetworkAvailableEv", nifm_available) { RET(0); }
HLE_FUNC("_ZN2nn4nifm19SetLocalNetworkModeEb", nifm_local_mode) { (void)c; }
HLE_FUNC("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_submit) { (void)c; ONCE("[offline] internet connection requested: none\n"); }
HLE_FUNC("_ZN2nn4nifm20CancelNetworkRequestEv", nifm_cancel) { (void)c; }
HLE_FUNC("_ZN2nn4nifm26HandleNetworkRequestResultEv", nifm_handle_result) { RET(NIFM_NOT_AVAILABLE); }
HLE_FUNC("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_ip_config) { RET(NIFM_NOT_AVAILABLE); }
HLE_FUNC("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_proxy) { RET(NIFM_NOT_AVAILABLE); }
HLE_FUNC("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_primary_ip) { RET(NIFM_NOT_AVAILABLE); }
