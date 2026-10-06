/* What a beacon or a probe response says of its network; see beacon.h. */

#include "utils/includes.h"

#include "utils/common.h"

#include "common/ieee802_11_common.h"

#include "beacon.h"

#define HEADER         24 /* frame control, duration, three addresses and the sequence number */
#define FIXED          12 /* timestamp, beacon interval and capabilities */
#define BSSID          16
#define BEACON         0x80
#define PROBE_RESPONSE 0x50

int beacon_read(const uint8_t *frame, uint32_t len, uint8_t heard_on, struct beacon *b)
{
    struct ieee802_11_elems elems;
    if (len < HEADER + FIXED || (frame[0] != BEACON && frame[0] != PROBE_RESPONSE) ||
        ieee802_11_parse_elems(frame + HEADER + FIXED, len - HEADER - FIXED, &elems, 0) == ParseFailed ||
        elems.ssid == NULL || elems.ssid_len > sizeof(b->ssid)) {
        return 0;
    }
    memcpy(b->bssid, frame + BSSID, 6);
    b->ssid_len = elems.ssid_len;
    memcpy(b->ssid, elems.ssid, elems.ssid_len);
    b->channel = elems.ds_params ? elems.ds_params[0] : heard_on;
    return 1;
}
