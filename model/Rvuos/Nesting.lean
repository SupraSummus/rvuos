/-
"Isolation" of design/verification.md, as `check_nesting` in `kernel/selfcheck.c` says it:
memory that may hold kernel objects, an Untyped or a pool,
overlaps another node standing for memory only where one lies below the other,
or where one of the two is an Untyped whose delete as a root has begun.
It is what keeps one block of memory from having two allocators.
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

/-- `going`: an Untyped whose delete as a root has begun, which makes roots of its children one by one. -/
def going (c : Cap) : Bool := c.type == capUntyped && c.b == untypedGoing

/-- Two nodes the nesting allows to stand for the same memory. -/
def nests (s : State) (x : Nat) (cx : Cap) (y : Nat) (cy : Cap) : Bool :=
  match s.range? cx, s.range? cy with
  | some (bx, zx), some (by_, zy) =>
    x == y || !(bx < by_ + zy && by_ < bx + zx) ||
      s.liesBelow x y || s.liesBelow y x || going cx || going cy
  | _, _ => true

/-- Memory that may hold kernel objects, an Untyped or a pool's own node. -/
def mayHold (c : Cap) : Bool := c.type == capUntyped || c.type == capRetyped

/--
What the nesting needs of "Derivation" to follow from step to step, as `kernel/selfcheck.c` checks it:
a pool's own node names a pool, and a frame, a NAPOT block, ends within 32 bits;
memory that may hold kernel objects lies right below an Untyped, if below anything,
and below a node that names an object lies only a node that names one, as `derived_from` says,
but for a pool's own node below another of its pool, which it lets through and nothing makes.
-/
def derivedOk (s : State) : Bool :=
  s.caps.all (fun (_, c) =>
    (c.type != capRetyped || (s.obj? c.a).map (·.type) == some capPool) &&
    (c.type != capFrame || decide (c.a + c.b ≤ 2 ^ 32))) &&
  s.kids.all fun (p, l) => l.all fun k =>
    match s.cap? p, s.cap? k with
    | some cp, some ck =>
      (!mayHold ck || cp.type == capUntyped) && (!capHasObject cp.type || capHasObject ck.type)
    | _, _ => true

/-- `check_nesting`: every Untyped and every pool nests with every node that stands for memory. -/
def nestingOk (s : State) : Bool :=
  s.caps.all fun (x, cx) =>
    !mayHold cx || s.caps.all fun (y, cy) => s.nests x cx y cy

end State

end Rvuos
