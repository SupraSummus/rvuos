/-
"Isolation" of design/verification.md, as `check_nesting` in `kernel/selfcheck.c` says it:
memory that may hold kernel objects, an Untyped or a pool,
overlaps another node standing for memory only where one lies below the other,
or where one of the two is a root Untyped that made something.
It is what keeps one block of memory from having two allocators.

The kernel does not keep it, and neither does the model: see `nesting_breaks` and `TODO.md`.
-/
import Rvuos.Step

namespace Rvuos

namespace State

/-- The memory a node stands for, `node_range`: a frame's, an installed region's, an Untyped's block, a pool's. -/
def range? (s : State) (c : Cap) : Option (Nat × Nat) :=
  if c.type == capFrame || c.type == capInstalled then some (c.a, c.b)
  else if c.type == capUntyped then
    some ((blockBase (BitVec.ofNat 32 c.a)).toNat, (blockSize (BitVec.ofNat 32 c.a)).toNat)
  else if c.type == capRetyped then (s.obj? c.a).map fun o => (c.a, o.size)
  else none

/-- `lies_below`: up from `n`, `a` within as many steps as there are nodes. -/
def liesBelow (s : State) (n a : Nat) : Bool :=
  go (s.caps.length + 1) n
where
  go : Nat → Nat → Bool
    | 0, _ => false
    | fuel + 1, k => match s.parent? k with
      | none => false
      | some p => p == a || go fuel p

/-- `made_root`: a root Untyped with something made of it below it. -/
def madeRoot (s : State) (n : Nat) (c : Cap) : Bool :=
  c.type == capUntyped && !(s.kidsOf n).isEmpty && (s.parent? n).isNone

/-- Two nodes the nesting allows to stand for the same memory. -/
def nests (s : State) (x : Nat) (cx : Cap) (y : Nat) (cy : Cap) : Bool :=
  match s.range? cx, s.range? cy with
  | some (bx, zx), some (by_, zy) =>
    x == y || !(bx < by_ + zy && by_ < bx + zx) ||
      s.liesBelow x y || s.liesBelow y x || s.madeRoot x cx || s.madeRoot y cy
  | _, _ => true

/-- `check_nesting`: every Untyped and every pool nests with every node that stands for memory. -/
def nestingOk (s : State) : Bool :=
  s.caps.all fun (x, cx) =>
    !(cx.type == capUntyped || cx.type == capRetyped) || s.caps.all fun (y, cy) => s.nests x cx y cy

end State

/-!
A root Untyped `U` whose delete stopped part way, as a delete of a root makes roots of its children one by one:
its lower half is a root already, its upper half still below it.
The half below goes, by a delete as here or by a revoke of `U`,
and `U` is free with its lower half's memory in it: two allocators over one block.
The thread at 1000 runs in the process at 2000, which mounts the table at 3000,
whose slot 0 names the table itself.
-/

def nestingMachine : Machine := ⟨8, 64, 12, 12, 184⟩

def nestingBefore : State where
  objs := [⟨1000, capThread, 0, 1184, 1, 0⟩, ⟨2000, capProcess, 0, 2012, 1, 0⟩, ⟨3000, capCaptable, 0, 3012, 4, 0⟩]
  caps := [(1184, ⟨capHosted, rightAll, 0, 2000, 0⟩),
    (2012, ⟨capCaptable, rightAll, 0, 3000, 0⟩),
    (3012, ⟨capCaptable, rightAll, 0, 3000, 0⟩),
    -- U, the block of 64 KiB at 0x10000, and its halves.
    (3036, ⟨capUntyped, rightAll, 0, 0x17fff, 0⟩),
    (3060, ⟨capUntyped, rightAll, 0, 0x1bfff, 0⟩),
    (3084, ⟨capUntyped, rightAll, 0, 0x13fff, 0⟩)]
  kids := [(3036, [3060])]

/-- `OP_CAP_DELETE` through slot 0 of the half in slot 2. -/
def nestingDelete : Event := .call 1000 opCapDelete [0, 2, 0, 0, 0]

/--
The nesting breaks.
Once the kernel keeps it, `model-check`'s search no longer breaks it and fails until a proof that every step keeps it replaces this,
which `nestingBefore`, a state the fix may leave unreachable, can make need a stronger statement.
-/
theorem nesting_breaks : Breaks State.nestingOk :=
  ⟨nestingMachine, nestingBefore, nestingDelete, by decide⟩

end Rvuos
