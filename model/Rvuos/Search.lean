/-
A breadth-first search over the model for a state that breaks an invariant,
from a state the kernel reached as an input's records began; see "The model" in DESIGN.md.

It follows every outcome the model names, each place a call may stop at among them,
which a fuzzer reaches only where an interrupt lands at that place.
A thread whose call stopped makes it again before anything else, as it does on the target,
while the others make any call they may.
The calls are those over capabilities to memory, an Untyped or a frame, which the nesting needs:
a capability of another type changes no node that stands for memory,
nor does a revoke below it, since what lies below it is of its type.
An invariant over other capabilities needs the search to follow theirs; see `TODO.md`.
Of the empty slots a call may fill, it tries the first two of a table, since empty slots differ only in their number.
-/
import Std.Data.HashSet
import Rvuos.Step

namespace Rvuos

namespace Search

/-- What a step of a search did: who made which call, and which of its outcomes it took. -/
structure Move where
  actor : Nat
  event : Event
  /-- The outcome taken, of those the model names. -/
  outcome : Nat
  stopped : Bool
  /-- A call the thread made again, having stopped in it. -/
  again : Bool

/-- A state of the search: the tree, the call each stopped thread makes again, and how it came. -/
structure Node where
  s : State
  pending : List (Nat × Event)
  path : List Move

def isMemory (c : Cap) : Bool := c.type == capUntyped || c.type == capFrame

/-- Every slot number of a thread's tables, with the node it names. -/
def slotsOf (m : Machine) (s : State) (t : Nat) : List (Nat × Nat) :=
  match threadProcess m s t with
  | none => []
  | some p => (List.range processTables).flatMap fun i =>
    match processTable m s p i with
    | none => []
    | some o => (List.range o.count).map fun idx => (i * 2 ^ slotTableShift + idx, nodeAt o idx)

/-- The calls a thread may make that the search tries. -/
def events (m : Machine) (s : State) (t : Nat) : List Event :=
  let slots := slotsOf m s t
  let filled := slots.filterMap fun (n, a) => (s.cap? a).map fun c => (n, c)
  let vacant := (slots.filter fun (_, a) => (s.cap? a).isNone).map (·.1)
  let mem := filled.filter (isMemory ·.2)
  let tables := filled.filterMap fun (n, c) => if c.type == capCaptable then (s.obj? c.a).map (n, ·) else none
  let onTables := tables.flatMap fun (a0, o) =>
    let idxs := List.range o.count
    let free := (idxs.filter fun i => (s.cap? (nodeAt o i)).isNone).take 2
    let held := idxs.filter fun i => ((s.cap? (nodeAt o i)).map isMemory).getD false
    (free.flatMap fun d => mem.flatMap fun (src, _) =>
      [.call t opCapCopy [a0, d, src, rightAll, 0], .call t opCapDerive [a0, d, src, rightAll, 0],
       .call t opCapMove [a0, d, src, 0, 0]]) ++
    (held.flatMap fun i => [.call t opCapDelete [a0, i, 0, 0, 0], .call t opCapRevoke [a0, i, 0, 0, 0]])
  let onUntyped := (mem.filter (·.2.type == capUntyped)).flatMap fun (a0, _) =>
    match vacant with
    | v1 :: v2 :: _ => [.call t opUntypedSplit [a0, v1, v2, 0, 0], .call t opUntypedRetype [a0, capFrame, v1, 0, 0]]
    | [v1] => [.call t opUntypedRetype [a0, capFrame, v1, 0, 0]]
    | [] => []
  onTables ++ onUntyped

def capHash (c : Cap) : UInt64 :=
  mixHash (hash c.type) (mixHash (hash c.rights) (mixHash (hash c.index) (mixHash (hash c.a) (hash c.b))))

/-- A hash of a node of the search, the same whatever order its state was built in. -/
def key (n : Node) : UInt64 :=
  let s := n.s.normal
  let h := s.caps.foldl (fun h (k, c) => mixHash h (mixHash (hash k) (capHash c))) 7
  let h := s.kids.foldl (fun h (k, l) => mixHash h (mixHash (hash k) (hash l))) h
  (n.pending.mergeSort (fun x y => x.1 ≤ y.1)).foldl (fun h (t, e) => mixHash h (mixHash (hash t)
    (match e with | .call _ op r => mixHash (hash op) (hash r) | .fault _ => 0))) h

/-- The first state, in breadth, within `depth` calls that a check fails on, with how it came. -/
def search (m : Machine) (start : State) (actors : List (Nat × Nat)) (depth : Nat) (bad : State → Bool) :
    Option Node := Id.run do
  let mut frontier : Array Node := #[⟨start, [], []⟩]
  let mut seen : Std.HashSet UInt64 := {}
  for _ in [0:depth] do
    let mut next : Array Node := #[]
    for n in frontier do
      for (actor, t) in actors do
        let (again, evs) := match n.pending.lookup t with
          | some e => (true, [e])
          | none => (false, events m n.s t)
        for e in evs do
          let some outs := step m n.s e | continue
          for (o, i) in outs.zipIdx do
            let stopped := match o.ret with | .again => true | _ => false
            let pending := (if stopped then [(t, e)] else []) ++ n.pending.filter (·.1 != t)
            let child : Node := ⟨o.st, pending, n.path ++ [⟨actor, e, i, stopped, again⟩]⟩
            let k := key child
            if seen.contains k then continue
            seen := seen.insert k
            if bad o.st then return some child
            next := next.push child
    frontier := next
  return none

def opName (op : Nat) : String :=
  if op == opCapCopy then "OP_CAP_COPY" else if op == opCapDerive then "OP_CAP_DERIVE"
  else if op == opCapMove then "OP_CAP_MOVE" else if op == opCapDelete then "OP_CAP_DELETE"
  else if op == opCapRevoke then "OP_CAP_REVOKE" else if op == opUntypedSplit then "OP_UNTYPED_SPLIT"
  else if op == opUntypedRetype then "OP_UNTYPED_RETYPE" else toString op

/--
A path as a seed for `tools/seed.py`, to replay on the host build:
a call that stops at the `n`th place it may is armed before with `OP_TEST_PREEMPT`,
and a call made again is the thread's own doing, which a comment marks.
-/
def seed (path : List Move) : List String :=
  path.flatMap fun mv =>
    match mv.event with
    | .fault _ => []
    | .call _ op r =>
      let call := s!"actor {mv.actor} {opName op} slot {r.getD 0 0} a1 {r.getD 1 0} a2 {r.getD 2 0} a3 {r.getD 3 0}"
      let place := if mv.stopped then s!", stopping at its place {mv.outcome + 1}" else ""
      if mv.again then [s!"# actor {mv.actor} makes its call again{place}: {call}"]
      else if mv.stopped then [s!"actor {mv.actor} OP_TEST_PREEMPT slot BOOT_CAP_TEST a1 {mv.outcome + 1}", call]
      else [call]

end Search

end Rvuos
