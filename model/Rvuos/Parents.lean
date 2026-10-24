/-
"Derivation" of design/verification.md, each node derived from its parent,
as `derived_from` in `kernel/selfcheck.c` says it.
-/
import Rvuos.Nesting

namespace Rvuos

/-- The kernel's type of a node: the model's own types for the nodes an object holds told back as the ABI's,
`capBound` a thread's units. -/
def kernelType (t : Nat) : Nat :=
  if t = capInstalled ∨ t = capRouted then capFrame
  else if t = capRetyped then capPool
  else if t = capBound then capTime
  else if t = capHosted then capProcess
  else if t = capSignalled ∨ t = capWatched then capNotification
  else t

/-- `!(x & ~y)`: no bit, of rights or of a notification's, that `y` lacks. -/
def sub (x y : Nat) : Bool := x &&& y == x

/-- `range_within`. -/
def within (b z pb pz : Nat) : Bool := b ≥ pb && b - pb ≤ pz - z && z ≤ pz

/-- A thread's units among those it was bound through, the first of them even for none. -/
def unitsWithin (f n pf pn : Nat) : Bool := f ≥ pf && f - pf < pn && n ≤ pn - (f - pf)

/-- `is_object_type`: a node naming an object built in a pool. -/
def isObjectType (t : Nat) : Bool :=
  t == capCaptable || t == capProcess || t == capThread || t == capNotification || t == capIrq || t == capRoute

/-- A type whose capability names nothing, `Print`, `Halt`, `Test` or the clock. -/
def namesNothing (t : Nat) : Bool := t == capPrint || t == capHalt || t == capTest || t == capClock

/-- `derived_from`: the node `c` may lie right below `p`. -/
def derivedFrom (s : State) (c p : Cap) : Bool :=
  let ct := kernelType c.type
  let pt := kernelType p.type
  -- What an Untyped made lies within it, and so does what came up to it from something it made that went;
  -- a pool's own node carries rights of its own.
  if p.type = capUntyped then
    match s.range? c, s.range? p with
    | some (b, z), some (pb, pz) =>
      (ct == capUntyped || ct == capFrame || c.type == capRetyped) &&
        (sub c.rights p.rights || c.type == capRetyped) && within b z pb pz
    | _, _ => false
  -- A node an object holds is of the type of the capability it hangs below.
  else if ct = pt then
    if ct = capFrame then sub c.rights p.rights && within c.a c.b p.a p.b
    else if c.type = capBound then sub c.rights p.rights && unitsWithin c.a c.b p.a p.b
    else if ct = capIrqLine ∨ ct = capTime ∨ ct = capRouteLine then sub c.rights p.rights && within c.a c.b p.a p.b
    else (sub c.rights p.rights || p.type == capRetyped) && (namesNothing ct || c.a == p.a) &&
      (ct != capNotification || sub c.b p.b)
  -- Below a pool's node, and below its capability, lies every capability to the pool's objects.
  else if pt = capPool then isObjectType ct && (s.obj? c.a).map (·.pool) == some p.a
  else false

namespace State

/--
A table holds capabilities of the ABI's types alone, none of the nodes an object holds,
as a node a table holds is no holder's: what the rule needs of a copy and a delete.
-/
def tablesOk (s : State) : Bool :=
  s.objs.all fun o => o.type != capCaptable || (List.range o.count).all fun i =>
    match s.cap? (nodeAt o i) with
    | some c => decide (c.type < capInstalled)
    | none => true

/-- Every node is derived from its parent, `derived_from` of `check_node`, and tables hold the ABI's types. -/
def parentsOk (s : State) : Bool :=
  s.tablesOk && s.kids.all fun (p, l) => l.all fun k =>
    match s.cap? p, s.cap? k with
    | some cp, some ck => derivedFrom s ck cp
    | _, _ => true

end State

end Rvuos
