/-
`model-check TRACE`: hold every step of a trace `host/model.c` wrote to the model.
A step the model says nothing of is counted by its operation;
a step whose outcome is none the model names, or a state that is no forest, fails the run.
-/
import Rvuos

open Rvuos

/-- The words of a line. -/
def words (l : String) : List String := ((l.replace "\n" "").splitOn " ").filter (· != "")

def nat! (w : String) : IO Nat :=
  match w.toNat? with
  | some n => pure n
  | none => throw (IO.userError s!"model-check: not a number: {w}")

def nats (ws : List String) : IO (List Nat) := ws.mapM nat!

/-- The lines of a trace, read one at a time. -/
structure Reader where
  h : IO.FS.Handle
  line : IO.Ref Nat

def Reader.next (r : Reader) : IO (List String) := do
  let l ← r.h.getLine
  r.line.modify (· + 1)
  if l.isEmpty then throw (IO.userError "model-check: the trace ends inside a step")
  pure (words l)

def expect (r : Reader) (tag : String) : IO (List String) := do
  let ws ← r.next
  if ws.head? == some tag then return ws.drop 1
  throw (IO.userError s!"model-check: line {← r.line.get}: {tag} expected")

/-- A state, from its `state` line to its `end`. -/
partial def readState (r : Reader) : IO State := do
  let _ ← expect r "state"
  let rec go (s : State) : IO State := do
    let ws ← r.next
    match ws with
    | ["end"] => pure { objs := s.objs.reverse, caps := s.caps.reverse, kids := s.kids.reverse }
    | "obj" :: rest =>
      let [a, t, p, n, c] ← nats rest | throw (IO.userError "model-check: obj takes five words")
      go { s with objs := ⟨a, t, p, n, c⟩ :: s.objs }
    | "node" :: rest =>
      let [n, t, rt, i, a, b] ← nats rest | throw (IO.userError "model-check: node takes six words")
      go { s with caps := (n, ⟨t, rt, i, a, b⟩) :: s.caps }
    | "kids" :: rest =>
      let (p :: ks) ← nats rest | throw (IO.userError "model-check: kids takes a parent")
      go { s with kids := (p, ks) :: s.kids }
    | _ => throw (IO.userError s!"model-check: line {← r.line.get}: obj, node, kids or end expected")
  go ⟨[], [], []⟩

/-- What a step left: the caller's registers, or what stood in for them. -/
inductive Seen where
  | ret (r : List Nat)
  | again
  | fault
  | gone

def readSeen (r : Reader) : IO Seen := do
  match ← expect r "result" with
  | "ret" :: rest => pure (.ret (← nats rest))
  | ["again"] => pure .again
  | ["fault"] => pure .fault
  | ["gone"] => pure .gone
  | _ => throw (IO.userError s!"model-check: line {← r.line.get}: a result expected")

def agrees (o : Outcome) (after : State) (seen : Seen) : Bool :=
  o.st.normal == after.normal &&
    match o.ret, seen with
    | .regs x, .ret y => x == y
    | .status k, .ret y => y.head? == some k
    | .again, .again => true
    | .fault, .fault => true
    | _, _ => false

def showSeen : Seen → String
  | .ret r => s!"ret {r}"
  | .again => "again"
  | .fault => "fault"
  | .gone => "gone"

def showRet : Ret → String
  | .regs r => s!"ret {r}"
  | .status k => s!"status {k}"
  | .again => "again"
  | .fault => "fault"

/-- What differs between two states, a line each, for the report. -/
def diff (want got : State) : List String :=
  let w := want.normal
  let g := got.normal
  let caps := (w.caps.filter (fun c => !g.caps.contains c)).map (fun c => s!"  model has  {repr c}") ++
    (g.caps.filter (fun c => !w.caps.contains c)).map (fun c => s!"  kernel has {repr c}")
  let kids := (w.kids.filter (fun c => !g.kids.contains c)).map (fun c => s!"  model has  kids {c}") ++
    (g.kids.filter (fun c => !w.kids.contains c)).map (fun c => s!"  kernel has kids {c}")
  let objs := if w.objs == g.objs then [] else ["  the objects differ"]
  caps ++ kids ++ objs

def main (args : List String) : IO UInt32 := do
  let [path] := args | do
    IO.eprintln "usage: model-check TRACE"
    return 2
  let h ← IO.FS.Handle.mk path .read
  let r : Reader := ⟨h, ← IO.mkRef 0⟩
  let mut steps := 0
  let mut checked := 0
  let mut failures := 0
  let mut unmodelled : List (Nat × Nat) := []
  repeat
    let l ← h.getLine
    if l.isEmpty then break
    r.line.modify (· + 1)
    let ws := words l
    let stepLine ← r.line.get
    let (kind, t) ← match ws with
      | ["step", k, t] => pure (k, ← nat! t)
      | _ => throw (IO.userError s!"model-check: line {stepLine}: step expected")
    let [rmin, pmin, pto, tso, tpo] ← nats (← expect r "machine") | throw (IO.userError "model-check: machine takes five words")
    let m : Machine := ⟨rmin, pmin, pto, tso, tpo⟩
    let regs ← nats (← expect r "op")
    let op := regs.headD 0
    let before ← readState r
    let seen ← readSeen r
    let after ← readState r
    steps := steps + 1
    for (s, which) in [(before, "before"), (after, "after")] do
      if !s.wellFormed then
        IO.eprintln s!"model-check: line {stepLine}: the state {which} the step is no forest"
        failures := failures + 1
    let e : Event := if kind == "call" then .call t op (regs.drop 1) else .fault t
    match step m before e with
    | none =>
      let n := (unmodelled.lookup op).getD 0
      unmodelled := (op, n + 1) :: unmodelled.filter (·.1 != op)
    | some outs =>
      checked := checked + 1
      if !outs.any (agrees · after seen) then
        failures := failures + 1
        if failures ≤ 10 then
          IO.eprintln s!"model-check: line {stepLine}: op {op} {regs.drop 1} by {t}: the kernel's outcome is not the model's"
          IO.eprintln s!"  kernel: {showSeen seen}"
          for o in outs do
            IO.eprintln s!"  model:  {showRet o.ret}"
            for d in diff o.st after do
              IO.eprintln d
  let ops := (unmodelled.mergeSort (fun x y => x.1 ≤ y.1)).map (fun (o, n) => s!"{o}:{n}")
  IO.println s!"model-check: {steps} steps, {checked} held to the model, {failures} failed"
  IO.println s!"model-check: unmodelled by operation: {String.intercalate " " ops}"
  return if failures == 0 then 0 else 1
