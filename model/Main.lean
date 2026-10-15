/-
`model-check [--search=N] TRACE`: hold every step of a trace `host/model.c` wrote to the model,
and every state in it to the invariants the model states; see "The model" in DESIGN.md.
A step the model says nothing of is counted by its operation;
a step whose outcome is none the model names, or a state that breaks an invariant, fails the run,
but for the state an invariant report found, which the kernel's self-check refused already.
With `--search=N` it then searches the model, N calls deep, from each state the trace says an input's records began in,
for a state that breaks an invariant, and gives the seed that reaches it;
that fails the run unless a theorem says a step breaks the invariant, a bug `TODO.md` lists,
and so does a search that no longer reaches a bug a theorem says is there, which a fix leaves behind.
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
      let [a, t, p, n, c, z] ← nats rest | throw (IO.userError "model-check: obj takes six words")
      go { s with objs := ⟨a, t, p, n, c, z⟩ :: s.objs }
    | "node" :: rest =>
      let [n, t, rt, i, a, b] ← nats rest | throw (IO.userError "model-check: node takes six words")
      go { s with caps := (n, ⟨t, rt, i, a, b⟩) :: s.caps }
    | "kids" :: rest =>
      let (p :: ks) ← nats rest | throw (IO.userError "model-check: kids takes a parent")
      go { s with kids := (p, ks) :: s.kids }
    | _ => throw (IO.userError s!"model-check: line {← r.line.get}: obj, node, kids or end expected")
  go ⟨[], [], []⟩

structure Invariant where
  name : String
  holds : State → Bool
  /-- That a step breaks it, a bug the kernel has. -/
  breaks : Option (PLift (Breaks holds))

/-- The properties of design/verification.md the model states. -/
def invariants : List Invariant :=
  [⟨"forest", State.wellFormed, none⟩,
   ⟨"nesting", State.nestingOk, none⟩]

/-- What a step left: the caller's registers, or what stood in for them. -/
inductive Seen where
  | ret (r : List Nat)
  | again
  | fault
  | gone

def readSeen (r : Reader) (ws : List String) : IO Seen := do
  match ws with
  | "ret" :: rest => pure (.ret (← nats rest))
  | ["again"] => pure .again
  | ["fault"] => pure .fault
  | ["gone"] => pure .gone
  | _ => throw (IO.userError s!"model-check: line {← r.line.get}: a result expected")

/-- What a step left, and whether an invariant report ended it. -/
def readResult (r : Reader) : IO (Bool × Seen) := do
  let ws ← r.next
  let (reported, ws) ← if ws == ["report"] then pure (true, ← r.next) else pure (false, ws)
  if ws.head? != some "result" then throw (IO.userError s!"model-check: line {← r.line.get}: result expected")
  pure (reported, ← readSeen r (ws.drop 1))

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

/-- The invariants a state breaks. -/
def broken (s : State) : List Invariant := invariants.filter (!·.holds s)

/-- A state the records of an input began in: the driver's threads that may run, by actor number, and the tree. -/
structure Start where
  actors : List (Nat × Nat)
  st : State

def pairs : List Nat → List (Nat × Nat)
  | a :: b :: rest => (a, b) :: pairs rest
  | _ => []

/-- Search from each start for a state that breaks each invariant; true if those that break are the known ones. -/
def searchAll (m : Machine) (starts : List Start) (depth : Nat) : IO Bool := do
  let mut ok := true
  for inv in invariants do
    let known := inv.breaks.isSome
    let say := if known then IO.println else IO.eprintln
    let mut found := false
    for st in starts do
      let some n := Search.search m st.st st.actors depth (!inv.holds ·) | continue
      found := true
      ok := ok && known
      say s!"model-check: {inv.name} breaks within {n.path.length} calls{if known then ", a known bug" else ""}; the seed:"
      for l in Search.seed n.path do
        say s!"  {l}"
      break
    if !found && known then
      ok := false
      IO.eprintln s!"model-check: no state within {depth} calls breaks {inv.name}, which a theorem says a step does: \
        once the kernel keeps it, the theorem goes"
    else if !found then
      IO.println s!"model-check: no state within {depth} calls breaks {inv.name}"
  return ok

def main (args : List String) : IO UInt32 := do
  let (opts, rest) := args.partition (·.startsWith "--")
  let depth := opts.findSome? fun o => (o.dropPrefix? "--search=").bind (·.toString.toNat?)
  let [path] := rest | do
    IO.eprintln "usage: model-check [--search=N] TRACE"
    return 2
  let h ← IO.FS.Handle.mk path .read
  let r : Reader := ⟨h, ← IO.mkRef 0⟩
  let mut steps := 0
  let mut checked := 0
  let mut failures := 0
  let mut unmodelled : List (Nat × Nat) := []
  let mut starts : List Start := []
  let mut startKeys : List UInt64 := []
  let mut machine : Option Machine := none
  repeat
    let l ← h.getLine
    if l.isEmpty then break
    r.line.modify (· + 1)
    let ws := words l
    let stepLine ← r.line.get
    if ws.head? == some "start" then
      let st ← readState r
      for inv in broken st do
        IO.eprintln s!"model-check: line {stepLine}: the state the records begin in breaks {inv.name}"
        failures := failures + 1
      let actors := pairs (← nats (ws.drop 1))
      let k := Search.key ⟨st, [], []⟩
      if !startKeys.contains k then
        startKeys := k :: startKeys
        starts := starts ++ [⟨actors, st⟩]
      continue
    let (kind, t) ← match ws with
      | ["step", k, t] => pure (k, ← nat! t)
      | _ => throw (IO.userError s!"model-check: line {stepLine}: step or start expected")
    let [rmin, pmin, pto, tso, tpo] ← nats (← expect r "machine") | throw (IO.userError "model-check: machine takes five words")
    let m : Machine := ⟨rmin, pmin, pto, tso, tpo⟩
    machine := some m
    let regs ← nats (← expect r "op")
    let op := regs.headD 0
    let before ← readState r
    let (reported, seen) ← readResult r
    let after ← readState r
    steps := steps + 1
    -- A state the self-check passed holds every invariant, or the model states one otherwise than the kernel.
    for (s, which) in (if reported then [(before, "before")] else [(before, "before"), (after, "after")]) do
      for inv in broken s do
        IO.eprintln s!"model-check: line {stepLine}: the state {which} the step breaks {inv.name}"
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
  let mut searched := true
  if let (some depth, some m) := (depth, machine) then
    searched ← searchAll m starts depth
  return if failures == 0 && searched then 0 else 1
