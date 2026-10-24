/-
The constants of `include/rvuos/abi.h` and `kernel/object.h` the model reads.
`model-check` holds nothing to the headers by itself:
a constant that drifts from its header shows as a step the model and the kernel disagree on.
-/

namespace Rvuos

/-! Capability types, `CAP_*`. -/
abbrev capNone : Nat := 0
abbrev capFrame : Nat := 1
abbrev capPool : Nat := 2
abbrev capCaptable : Nat := 3
abbrev capProcess : Nat := 4
abbrev capThread : Nat := 5
abbrev capPrint : Nat := 6
abbrev capNotification : Nat := 7
abbrev capUntyped : Nat := 8
abbrev capIrqLine : Nat := 9
abbrev capIrq : Nat := 10
abbrev capClock : Nat := 11
abbrev capTime : Nat := 12
abbrev capHalt : Nat := 13
abbrev capTest : Nat := 14
abbrev capRouteLine : Nat := 15
abbrev capRoute : Nat := 16
abbrev capDmaLine : Nat := 17
abbrev capDma : Nat := 18
/-- The nodes an object holds, by types of the model's own, never in a table; see `put_node` in `host/model.c`. -/
abbrev capInstalled : Nat := 0x80
abbrev capRetyped : Nat := 0x81
abbrev capBound : Nat := 0x82
abbrev capHosted : Nat := 0x83
abbrev capSignalled : Nat := 0x84
abbrev capWatched : Nat := 0x85
abbrev capRouted : Nat := 0x86
abbrev capChannelled : Nat := 0x87

/-! Statuses, `KERR_*`. -/
abbrev kOk : Nat := 0
abbrev kInvalidCap : Nat := 1
abbrev kWrongType : Nat := 2
abbrev kNoRights : Nat := 3
abbrev kInvalidArg : Nat := 4
abbrev kNoMemory : Nat := 5
abbrev kSlotInUse : Nat := 6
abbrev kState : Nat := 9

/-! Rights, `RIGHT_*`. -/
abbrev rightR : Nat := 1
abbrev rightW : Nat := 2
abbrev rightAll : Nat := 7

/-! Operations, `OP_*`. -/
abbrev opCapCopy : Nat := 3
abbrev opCapDelete : Nat := 4
abbrev opFrameCarve : Nat := 5
abbrev opUntypedRetype : Nat := 6
abbrev opCapMove : Nat := 18
abbrev opCapRevoke : Nat := 23
abbrev opCapDerive : Nat := 24
abbrev opUntypedSplit : Nat := 30
abbrev opCapInfo : Nat := 41
abbrev opCount : Nat := 53

/-! Slot numbers, `SLOT_TABLE_SHIFT` and `PROCESS_TABLES`. -/
abbrev slotTableShift : Nat := 10
abbrev processTables : Nat := 2
/-- A node, `struct cap`, is 16 bytes. -/
abbrev nodeSize : Nat := 16

/-- True if a capability of this type names a kernel object, `cap_has_object`. -/
def capHasObject (t : Nat) : Bool :=
  !(t == capNone || t == capFrame || t == capUntyped || t == capIrqLine || t == capPrint ||
    t == capHalt || t == capTest || t == capClock || t == capInstalled || t == capTime || t == capBound ||
    t == capRouteLine || t == capRouted || t == capDmaLine || t == capChannelled)

/-! What dispatch checks before a handler, `op_shapes` in `kernel/syscall.c`. -/
abbrev shapeA1 : Nat := 0x1
abbrev shapeA2 : Nat := 0x2
abbrev shapeA3 : Nat := 0x4
abbrev shapeA123 : Nat := 0x7
abbrev shapeRightsA3 : Nat := 0x8
abbrev shapeVaries : Nat := 0x10

structure Shape where
  type : Nat
  right : Nat
  reads : Nat

/-- `op_shapes`, by operation code; a code with no entry has type `CAP_NONE`. -/
def shapeOf : Nat → Shape
  | 1 => ⟨capPrint, rightW, shapeA123⟩
  | 2 => ⟨capHalt, rightW, shapeA1⟩
  | 10 => ⟨capTest, rightW, 0⟩
  | 17 => ⟨capTest, rightW, 0⟩
  | 22 => ⟨capTest, rightW, shapeA1⟩
  | 32 => ⟨capTest, rightW, shapeA1⟩
  | 3 => ⟨capCaptable, rightW, shapeA123 ||| shapeRightsA3⟩
  | 4 => ⟨capCaptable, rightW, shapeA1⟩
  | 23 => ⟨capCaptable, rightW, shapeA1⟩
  | 24 => ⟨capCaptable, rightW, shapeA123 ||| shapeRightsA3⟩
  | 18 => ⟨capCaptable, rightW, shapeA1 ||| shapeA2⟩
  | 5 => ⟨capFrame, 0, shapeA123⟩
  | 6 => ⟨capUntyped, 0, shapeA1 ||| shapeA2⟩
  | 30 => ⟨capUntyped, 0, shapeA1 ||| shapeA2⟩
  | 7 => ⟨capPool, rightW, shapeA123 ||| shapeVaries⟩
  | 42 => ⟨capPool, 0, shapeA1 ||| shapeA3⟩
  | 8 => ⟨capProcess, rightW, shapeA123 ||| shapeRightsA3⟩
  | 9 => ⟨capProcess, rightW, shapeA1⟩
  | 39 => ⟨capProcess, rightW, shapeA1 ||| shapeA2⟩
  | 40 => ⟨capProcess, rightW, shapeA1⟩
  | 12 => ⟨capThread, rightW, shapeA123⟩
  | 13 => ⟨capThread, rightW, 0⟩
  | 16 => ⟨capThread, rightW, 0⟩
  | 31 => ⟨capThread, rightW, shapeA1 ||| shapeA2 ||| shapeVaries⟩
  | 33 => ⟨capThread, rightW, 0⟩
  | 34 => ⟨capThread, rightW, shapeA1⟩
  | 35 => ⟨capThread, rightW, shapeA1 ||| shapeA2⟩
  | 47 => ⟨capThread, rightR, 0⟩
  | 14 => ⟨capNotification, rightW, shapeA1⟩
  | 15 => ⟨capNotification, rightR, shapeA1⟩
  | 38 => ⟨capNotification, rightR, shapeA1 ||| shapeA2⟩
  | 36 => ⟨capNotification, 0, shapeA1 ||| shapeA2⟩
  | 19 => ⟨capIrqLine, 0, shapeA123⟩
  | 20 => ⟨capIrqLine, rightW, shapeA123⟩
  | 21 => ⟨capIrq, rightW, shapeA123 ||| shapeVaries⟩
  | 25 => ⟨capClock, rightR, 0⟩
  | 37 => ⟨capClock, rightW, shapeA1⟩
  | 28 => ⟨capTime, 0, shapeA123⟩
  | 29 => ⟨capTime, rightW, shapeA123⟩
  | 43 => ⟨capRouteLine, 0, shapeA123⟩
  | 44 => ⟨capRouteLine, rightW, shapeA1 ||| shapeA2⟩
  | 45 => ⟨capRoute, rightW, shapeA123⟩
  | 46 => ⟨capRoute, rightW, shapeA1⟩
  | 48 => ⟨capDmaLine, 0, shapeA123⟩
  | 49 => ⟨capDmaLine, rightW, shapeA1 ||| shapeA2⟩
  | 50 => ⟨capDma, rightW, shapeA123⟩
  | 51 => ⟨capDma, rightW, shapeA1 ||| shapeA2⟩
  | 52 => ⟨capDma, rightW, 0⟩
  | _ => ⟨capNone, 0, 0⟩

end Rvuos
