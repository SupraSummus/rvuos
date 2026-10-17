/-
What one call does to the tree, as `kernel/syscall.c` and `kernel/cap.c` do it:
every outcome the kernel may reach, more than one where a call may stop part way,
or `none` where the model says nothing; see DESIGN.md, "The model", for which operations it covers.
-/
import Rvuos.State

namespace Rvuos

/-- What the caller's registers hold after a step. -/
inductive Ret where
  /-- a0 to a4 as the call returns them. -/
  | regs (r : List Nat)
  /-- a0 as the call returns it, the rest not modelled. -/
  | status (k : Nat)
  /-- Stopped, to be made again: the registers as the call found them. -/
  | again
  /-- A fault, which writes no register. -/
  | fault
deriving BEq, Repr

structure Outcome where
  st : State
  ret : Ret

inductive Event where
  /-- A call by the thread at `t`: a7, then a0 to a4. -/
  | call (t op : Nat) (r : List Nat)
  | fault (t : Nat)

/-- A handler's answer: a refusal, which leaves the state as it was, the outcomes, or unmodelled. -/
abbrev Answer := Except Nat (Option (List Outcome))

/-- The `i`th of a list of registers, 0 past its end. -/
def reg (r : List Nat) (i : Nat) : Nat := r.getD i 0

section
variable (m : Machine) (s : State)

/-- The node at index `idx` of an object's nodes. -/
def nodeAt (o : Obj) (idx : Nat) : Nat := o.nodes + nodeSize * idx

/-- `thread_process`: the process a thread runs in, by its hosted node. -/
def threadProcess (t : Nat) : Option Nat := do
  let c ← s.cap? (t + m.threadProcOff)
  if c.type == capHosted then some c.a else none

/-- `process_table`: the table a process mounts in table slot `i`. -/
def processTable (p i : Nat) : Option Obj := do
  let c ← s.cap? (p + m.procTablesOff + nodeSize * i)
  guard (c.type == capCaptable)
  let o ← s.obj? c.a
  guard (o.type == capCaptable)
  pure o

def slotIndex (slot : Nat) : Nat := slot % 2 ^ slotTableShift

/-- `slot_table`: the table a slot number of a thread's points into. -/
def slotTable (t slot : Nat) : Option Obj := do
  let p ← threadProcess m s t
  let i := slot / 2 ^ slotTableShift
  if i < processTables then processTable m s p i else none

/-- `cap_lookup`. -/
def capLookup (o : Obj) (idx : Nat) : Except Nat Cap :=
  if idx ≥ o.count then .error kInvalidCap
  else match s.cap? (nodeAt o idx) with
    | none => .error kInvalidCap
    | some c =>
      if capHasObject c.type && (s.obj? c.a).map (·.type) != some c.type then .error kInvalidCap
      else .ok c

/-- `slot_lookup`: the node a slot number names and its words. -/
def slotLookup (t slot : Nat) : Except Nat (Nat × Cap) := do
  let some o := slotTable m s t slot | throw kInvalidCap
  let c ← capLookup s o (slotIndex slot)
  pure (nodeAt o (slotIndex slot), c)

/-- `cap_slot_free`: the empty node at an index of a table. -/
def capSlotFree (o : Obj) (idx : Nat) : Except Nat Nat :=
  if idx ≥ o.count then .error kInvalidCap
  else if (s.cap? (nodeAt o idx)).isSome then .error kSlotInUse
  else .ok (nodeAt o idx)

/-- `slot_vacant`: the empty node a slot number names. -/
def slotVacant (t slot : Nat) : Except Nat Nat := do
  let some o := slotTable m s t slot | throw kInvalidCap
  capSlotFree s o (slotIndex slot)

/-- `slot_at`: the node a slot number names, filled or not. -/
def slotAt (t slot : Nat) : Option Nat := do
  let o ← slotTable m s t slot
  if slotIndex slot < o.count then some (nodeAt o (slotIndex slot)) else none

/-- `range_contains`, in 32 bits. -/
def rangeContains (base size x : Nat) : Bool :=
  (BitVec.ofNat 32 x - BitVec.ofNat 32 base).toNat < size

/-- `holds_caller`: an Untyped's memory holds what the caller runs on, or the invoked table. -/
def holdsCaller (t : Nat) (named : Obj) (base size : Nat) : Bool :=
  let poolOf (a : Nat) : Option Nat := (s.obj? a).map (·.pool)
  let inside (a : Option Nat) : Bool := match a with
    | some x => rangeContains base size x
    | none => false
  let p := threadProcess m s t
  inside (poolOf t) || inside (p.bind poolOf) || rangeContains base size named.pool ||
    (List.range processTables).any (fun i => inside ((p.bind (processTable m s · i)).map (·.pool)))

/-- `shape_check`: the type, the right, and zero in what the operation does not read; `none` where its values decide. -/
def shapeCheck (op : Nat) (c : Cap) (r : List Nat) : Except Nat (Option Unit) :=
  let sh := shapeOf op
  if sh.type == capNone || sh.type != c.type then .error kWrongType
  else if c.rights &&& sh.right != sh.right then .error kNoRights
  else if sh.reads &&& shapeVaries != 0 then .ok none
  else
    let unread := (if sh.reads &&& shapeA1 != 0 then 0 else reg r 1) |||
      (if sh.reads &&& shapeA2 != 0 then 0 else reg r 2) |||
      (if sh.reads &&& shapeA3 != 0 then 0 else reg r 3) |||
      (if sh.reads &&& shapeRightsA3 != 0 then reg r 3 >>> 3 else 0)
    if unread != 0 then .error kInvalidArg else .ok (some ())

/-- A finished call: status zero and the arguments as they came, but where a handler writes a result. -/
def finish (st : State) (r : List Nat) : Outcome := ⟨st, .regs (kOk :: r.drop 1)⟩

/-- `untyped_free`: nothing made of the Untyped at `u` is left below it, and its delete as a root has not begun. -/
def untypedFree (u : Nat) (c : Cap) : Bool := (s.kidsOf u).isEmpty && c.b != untypedGoing

/-- A node filled with words below a parent, or a root. -/
def store (n : Nat) (c : Cap) (parent : Option Nat) : State := (s.setCap n c).attach parent n

end

/-- `cap_revoke_below`'s loop: clear the root's first child until none is left, a step a node. -/
def revokeLoop (m : Machine) (t slot root : Nat) (r : List Nat) :
    Nat → State → List Outcome → Option (List Outcome)
  | 0, _, _ => none
  | fuel + 1, s, acc =>
    match s.kidsOf root with
    | [] => some (acc ++ [finish s r])
    | n :: _ =>
      match s.cap? n with
      | none => none
      | some c =>
        -- A pool's own node: the pool is destroyed below it, which the model leaves out.
        if c.type == capRetyped then none
        else
          let s' := (s.detachAdopt n).eraseCap n
          -- A revoke that took what the call was made through ends there.
          let gone := match slotAt m s' t slot with
            | none => true
            | some x => (s'.cap? x).isNone
          if gone then some (acc ++ [finish s' r])
          else if (s'.kidsOf root).isEmpty then revokeLoop m t slot root r fuel s' acc
          else revokeLoop m t slot root r fuel s' (acc ++ [⟨s', .again⟩])

/-- `cap_delete` of a root: each child becomes a root, a step each, before the node goes. -/
def deleteRoot (n : Nat) (r : List Nat) : List Nat → State → List Outcome → List Outcome
  | [], s, acc => acc ++ [finish (s.eraseCap n) r]
  | [_], s, acc => acc ++ [finish ((s.setKids n []).eraseCap n) r]
  | _ :: k :: ks, s, acc =>
    let s' := s.setKids n (k :: ks)
    deleteRoot n r (k :: ks) s' (acc ++ [⟨s', .again⟩])

section
variable (m : Machine) (s : State)

/-- `OP_CAP_COPY`, or `OP_CAP_DERIVE`: beside the source, or below it. -/
def capCopy (t : Nat) (derive : Bool) (table : Obj) (r : List Nat) : Answer := do
  let (source, src) ← slotLookup m s t (reg r 2)
  let src := { src with rights := src.rights &&& (reg r 3 % 256) }
  if derive then
    if src.type == capUntyped && !untypedFree s source src then throw kNoMemory
    let dst ← capSlotFree s table (reg r 1)
    pure (some [finish (store s dst src (some source)) r])
  else
    if src.type == capUntyped then throw kWrongType
    let dst ← capSlotFree s table (reg r 1)
    pure (some [finish ((s.setCap dst src).attachBeside source dst) r])

/-- `OP_CAP_MOVE`. -/
def capMove (t : Nat) (table : Obj) (r : List Nat) : Answer := do
  let (source, _) ← slotLookup m s t (reg r 2)
  let dst ← capSlotFree s table (reg r 1)
  pure (some [finish (s.move source dst) r])

/-- `OP_CAP_DELETE`: an empty slot is cleared already, and a root goes. -/
def capDelete (table : Obj) (r : List Nat) : Answer :=
  if reg r 1 ≥ table.count then .error kInvalidCap
  else
    let n := nodeAt table (reg r 1)
    match s.cap? n with
    | none => .ok (some [finish s r])
    | some c => match s.parent? n with
      | some _ => .ok (some [finish ((s.detachAdopt n).eraseCap n) r])
      | none => .ok (some (deleteRoot n r (s.kidsOf n) (s.setCap n c.markGoing) []))

/-- `OP_CAP_REVOKE`: below an Untyped that holds what the caller runs on, none. -/
def capRevoke (t slot : Nat) (table : Obj) (r : List Nat) : Answer :=
  match capLookup s table (reg r 1) with
  | .error k => .error k
  | .ok c =>
    let a := BitVec.ofNat 32 c.a
    if c.type == capUntyped && holdsCaller m s t table (blockBase a).toNat (blockSize a).toNat then .error kState
    else .ok (revokeLoop m t slot (nodeAt table (reg r 1)) r (s.caps.length + 1) s [])

/-- A capability table: copy, derive, move, delete and revoke. -/
def opCaptable (t slot op : Nat) (table : Obj) (r : List Nat) : Answer :=
  if op == opCapCopy then capCopy m s t false table r
  else if op == opCapDerive then capCopy m s t true table r
  else if op == opCapMove then capMove m s t table r
  else if op == opCapDelete then capDelete s table r
  else if op == opCapRevoke then capRevoke m s t slot table r
  else .ok none

/-- A frame: a carve. -/
def opFrame (t op node : Nat) (c : Cap) (r : List Nat) : Answer :=
  if op != opFrameCarve then .ok none
  else do
    let off := reg r 1
    let len := reg r 2
    let base := BitVec.ofNat 32 c.a + BitVec.ofNat 32 off
    if off ≥ c.b || len > c.b - off || !napot m.regionMin base (BitVec.ofNat 32 len) then throw kInvalidArg
    let dst ← slotVacant m s t (reg r 3)
    pure (some [finish (store s dst ⟨capFrame, c.rights, 0, base.toNat, len⟩ (some node)) r])

/-- `OP_UNTYPED_RETYPE`, into a frame; into a pool it builds objects, which the model leaves out. -/
def untypedRetype (t u : Nat) (c : Cap) (r : List Nat) : Answer := do
  let a := BitVec.ofNat 32 c.a
  let type := reg r 1
  if type != capFrame && type != capPool then throw kInvalidArg
  if type == capPool && c.rights &&& (rightR ||| rightW) != (rightR ||| rightW) then throw kNoRights
  if type == capPool && (blockSize a).toNat < m.poolMin then throw kInvalidArg
  let dst ← slotVacant m s t (reg r 2)
  if !untypedFree s u c then throw kNoMemory
  if type == capPool then pure none
  else
    let st := store s dst ⟨capFrame, c.rights, 0, (blockBase a).toNat, (blockSize a).toNat⟩ (some u)
    pure (some [⟨st, .regs [kOk, (blockBase a).toNat, reg r 2, reg r 3, reg r 4]⟩])

/-- `OP_UNTYPED_SPLIT`: two halves below it, the lower first. -/
def untypedSplit (t u : Nat) (c : Cap) (r : List Nat) : Answer := do
  let a := BitVec.ofNat 32 c.a
  let base := blockBase a
  let half := blockSize a / 2
  if !napot m.regionMin base half then throw kInvalidArg
  let lo ← slotVacant m s t (reg r 1)
  let hi ← slotVacant m s t (reg r 2)
  if lo == hi then throw kInvalidArg
  if !untypedFree s u c then throw kNoMemory
  let s1 := store s lo ⟨capUntyped, c.rights, 0, (blockWord base half).toNat, 0⟩ (some u)
  let s2 := store s1 hi ⟨capUntyped, c.rights, 0, (blockWord (base + half) half).toNat, 0⟩ (some u)
  pure (some [finish s2 r])

/-- An Untyped: a split, and a retype. -/
def opUntyped (t op u : Nat) (c : Cap) (r : List Nat) : Answer :=
  if op == opUntypedRetype then untypedRetype m s t u c r
  else if op == opUntypedSplit then untypedSplit m s t u c r
  else .ok none

/-- One step: every outcome the kernel may reach, or `none` where the model does not say. -/
def step : Event → Option (List Outcome)
  | .fault _ => some [⟨s, .fault⟩]
  | .call t op r =>
    let refuse (k : Nat) : Option (List Outcome) := some [⟨s, .regs (k :: r.drop 1)⟩]
    let slot := reg r 0
    match slotTable m s t slot with
    | none => refuse kInvalidCap
    | some table =>
      match capLookup s table (slotIndex slot) with
      | .error k => refuse k
      | .ok c =>
        -- Describing changes nothing, and what it tells is left out but whether it told.
        if op == opCapInfo then
          if reg r 1 != 0 || reg r 2 != 0 || reg r 3 != 0 then refuse kInvalidArg
          else some [⟨s, .status kOk⟩]
        else match shapeCheck op c r with
          | .error k => refuse k
          | .ok none => none
          | .ok (some ()) =>
            let node := nodeAt table (slotIndex slot)
            let h : Answer :=
              if c.type == capCaptable then
                match s.obj? c.a with
                | some named => opCaptable m s t slot op named r
                | none => .ok none
              else if c.type == capFrame then opFrame m s t op node c r
              else if c.type == capUntyped then opUntyped m s t op node c r
              else .ok none
            match h with
            | .error k => refuse k
            | .ok outs => outs

end

/-- A step breaks an invariant: from a forest that holds it, to an outcome that does not. -/
def Breaks (inv : Machine → State → Bool) : Prop :=
  ∃ m s e, s.wellFormed = true ∧ inv m s = true ∧ ((step m s e).getD []).any (fun o => !inv m o.st) = true

end Rvuos
