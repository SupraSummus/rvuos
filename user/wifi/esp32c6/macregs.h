#ifndef RVUOS_WIFI_MACREGS_H
#define RVUOS_WIFI_MACREGS_H

/*
 * The MAC's and the HAL's registers, and how the driver reaches them,
 * shared by mac.c, which receives and sends once the libraries have brought the MAC up, and macstart.c, the register
 * sequences of that bring-up. The MAC's block is at 0x600a4000 and the HAL's at 0x600a4c00, with the crypto's and the
 * coex PTI's beside them; what a word or a bit is, where it is not evident, is said in mac.c or macstart.c.
 */

#include <stdint.h>

#define MAC_BASE 0x600a4000u

#define RX_CTRL        (MAC_BASE + 0x080u)
#define RX_CTRL_RELOAD 0x1u
#define RX_CTRL_ENABLE 0x80000000u
#define RX_CTRL_HW_BEACON_RELOAD 0x08000000u /* the receive buffer's reload by the hardware beacon; see macstart.c */
#define RX_BASE        (MAC_BASE + 0x084u) /* the descriptor list the MAC fills, which mac.c points at the driver's own */
#define RX_WORD        (MAC_BASE + 0x98u)

#define HAL_CTRL 0x600a4308u /* hal_deinit's four; what they are is pp's, and no driver code reads them */
#define HAL_HOLD 0x600a4c40u
#define HAL_MISC 0x600a4c4cu
#define HAL_CFG  0x600a4ddcu
#define HAL_WORD 0x600a4c8cu /* whose set bits, and RX_WORD's, the libraries' hal_init sets too; the bits' meaning is not known */

#define PTI_RX      0x600a42fcu /* what hal_set_rx_active_pti and hal_set_rx_ack_pti clear */
#define PTI_DEFAULT 0x600a4dd8u /* its low nibble, which hal_set_wifi_default_pti sets */

/*
 * The MAC's own station address and the access point's; the flags are what hal_mac_set_addr and hal_mac_set_bssid set.
 * The driver writes them when its station associates, and mac_stop makes them invalid when it leaves.
 * Interface 1's own word by the bssid's, IF1_DEFAULT_WORD, the libraries' default receive policy gives bit 30,
 * IF1_DEFAULT_BIT, and no other code the driver reads writes.
 */
#define BSSID_LO       (MAC_BASE + 0x00u)
#define BSSID_HI       (MAC_BASE + 0x04u)
#define STA_ADDR_LO    (MAC_BASE + 0x5cu)
#define STA_ADDR_HI    (MAC_BASE + 0x60u)
#define STA_ADDR2_LO   (MAC_BASE + 0x64u) /* interface 1's, the soft AP's, which hal_mac_set_addr's index 1 sets */
#define STA_ADDR2_HI   (MAC_BASE + 0x68u)
#define BSSID_FLAG     0x80000000u
#define BSSID_HI_OTHER 0x00040000u /* their default policy clears it; the bit's meaning is not known */
#define BSSID_HI_LOW   0x00000001u /* their default policy clears it too, in the bssid's word and interface 1's */
#define STA_ADDR_FLAG  0x00010000u /* what hal_mac_set_addr ors: lui 0x10, not 0x100000 */
#define IF1_DEFAULT_WORD (MAC_BASE + 0x0cu)
#define IF1_DEFAULT_BIT  0x40000000u

#define RX_POLICY           (MAC_BASE + 0x0d8u) /* interface 0's; interface n's 4 * n further */
#define RX_POLICY1          (RX_POLICY + 4u)    /* interface 1's, which the default policy writes too */
#define RX_POLICY_CLEAR     0x00000450u         /* cleared by a policy of 1 and 1, as the station's */
#define RX_POLICY_QUEUE     0x00000102u         /* set by hal_mac_set_rxq_policy's 1 */
#define BSSID_HI_POLICY     0x40000000u         /* cleared by the policy, beside BSSID_FLAG, which it sets */

#define SNIFF_CTRL0           (MAC_BASE + 0x0f8u)
#define SNIFF_CTRL1           (MAC_BASE + 0x0fcu)
#define SNIFF_CTRL_NONE       0x05000000u
#define SNIFF_CTRL_TYPES      (MAC_BASE + 0x104u)
#define SNIFF_CTRL_TYPES_MASK 0xffff0000u

/*
 * A register read and write, volatile, as the driver makes them; on the host, for the bring-up's replay,
 * test/mac-replay-test.c gives its own, which answers from the libraries' recorded accesses.
 */
#ifndef MAC_HOST
static inline uint32_t rd(uint32_t a)
{
    return *(volatile uint32_t *)a;
}

static inline void wr(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}
#else
uint32_t rd(uint32_t a);
void wr(uint32_t a, uint32_t v);
#endif

#endif
