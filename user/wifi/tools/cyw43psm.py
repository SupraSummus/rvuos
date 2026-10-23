"""D11's processor, the PSM, from the host: the few of its instructions our microcode uses, and loading them; see user/wifi/hw/d11.md.

An instruction is an opcode, from bit 39, and three operands of 13 bits, the first two read and the last written;
the microcode memory holds each as two words, its low 32 bits first.
An operand of 13 bits is, by its value:
0 to 0xfff a halfword of the shared memory;
0x1000 to 0x13ff an internal register, IHR, whose halfword D11's page shows at 0x400 + 2n;
0x1400 + (k << 7) + i the shared memory's halfword i past psm_base_k, the IHR 0x60 + k;
0x1780 + n the scratch register rn;
0x1800 to 0x1fff an immediate of 11 bits, signed.
"""

import time

MACCONTROL, MACINTSTATUS = 0x120, 0x128  # in D11's page
MAC_SUSPENDED = 1  # macintstatus's MI_MACSSPNDD, written 1 to clear
BTCX_TRANSCTL = 0x400 + 2 * 0x15C  # the IHR of Bluetooth's coexistence through which the WLAN holds the shared front end
OBJADDR, OBJDATA = 0x160, 0x164
OBJ_MICROCODE, OBJ_AUTO_INC = 0, 3 << 24
# maccontrol: the MAC's enable, the PSM's run, its jump to 0, the internal registers' enable, and wake
MAC_ENABLE, PSM_RUN, PSM_JUMP_0, IHR_ENABLE, WAKE = 1, 1 << 1, 1 << 2, 1 << 10, 1 << 26
HALTED = IHR_ENABLE | PSM_JUMP_0 | WAKE
RUNNING = IHR_ENABLE | PSM_RUN | WAKE


def imm(value):
    assert -0x400 <= value < 0x400
    return 0x1800 + (value & 0x7FF)


def ihr(n):
    assert 0 <= n < 0x400
    return 0x1000 + n


def r(n):
    assert 0 <= n < 0x80
    return 0x1780 + n


def indirect(base, i):
    """The shared memory's halfword i past psm_base_<base>."""
    assert 0 <= base < 7 and 0 <= i < 0x80
    return 0x1400 + (base << 7) + i


EXT_ADDR, EXT_DATA = ihr(0x18), ihr(0x19)  # the PHY's registers: an address with 0x4000 writes, 0x6000 reads; bit 14 busy
BASE_0 = ihr(0x60)


def ins(op, a, b, c):
    return op << 39 | a << 26 | b << 13 | c


def mov16(value, dst):
    """dst = value, any 16 bits: orx of 8 bits, the high byte rotated over the low."""
    return ins(0x378, imm(value >> 8), imm(value & 0xFF), dst)


def mov(src, dst):
    return ins(0x160, src, imm(0), dst)  # or src, 0


def add(a, b, dst):
    return ins(0x1C0, a, b, dst)


def jne(a, b, to):
    return ins(0x0D1, a, b, to)


def jmp(to):
    return ins(0x77F, r(0), r(0), to)  # jext on the condition always true


def orx(width, shift, a, b, dst):
    """dst = a's low width bits rotated left by shift, over b's other bits."""
    return ins(0x300 | (width - 1) << 4 | shift, a, b, dst)


def jnzx(shift, width, a, to):
    """Jump to `to` if a's width bits from shift are not all 0; to itself, a wait while they are not."""
    return ins(0x500 | (width - 1) << 4 | shift, a, imm(0), to)


def words(program):
    return [w for v in program for w in (v & 0xFFFFFFFF, v >> 32)]


def suspend(board, d11, seconds=2.0):
    """The MAC suspended as brcmsmac suspends it, by its microcode, which finishes what it was receiving first;
    the microcode then gives the front end up to Bluetooth, BTCX_TRANSCTL's bits 6 and 7 cleared, which this puts back."""
    control = board.read(d11 + MACCONTROL)
    if not control & MAC_ENABLE or not control & PSM_RUN:
        return True
    coexistence = board.read16(d11 + BTCX_TRANSCTL)
    board.write(d11 + MACINTSTATUS, MAC_SUSPENDED)
    board.write(d11 + MACCONTROL, control & ~MAC_ENABLE)
    end = time.time() + seconds
    while not board.read(d11 + MACINTSTATUS) & MAC_SUSPENDED:
        if time.time() > end:
            return False
    board.write16(d11 + BTCX_TRANSCTL, coexistence)
    return True


def load(board, d11, program):
    """The MAC suspended, the PSM halted at 0 and program put in its microcode memory;
    False if the MAC did not suspend or the program does not read back."""
    if not suspend(board, d11):
        return False
    board.write(d11 + MACCONTROL, HALTED)
    w = words(program)
    board.write(d11 + OBJADDR, OBJ_AUTO_INC | OBJ_MICROCODE)
    for x in w:
        board.write(d11 + OBJDATA, x)
    board.write(d11 + OBJADDR, OBJ_AUTO_INC | OBJ_MICROCODE)
    return [board.read(d11 + OBJDATA) for _ in w] == w
